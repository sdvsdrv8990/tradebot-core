#define TB_CURRENT_GROUP TB_G_SCHED /* зона ответственности файла */
/* ==========================================================================
 *  tb/sched/loop.c — событийный цикл: epoll + eventfd + таймерное колесо.
 * ========================================================================== */
#include <stdio.h>
#include "tb/sched/loop.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "tb/core/log.h"
#include "tb/core/time.h"
#include "tb/sched/threadpool.h"

/* Замыкание кросс-поточного постинга; дренируется циклом. */
typedef struct tb_incoming {
    tb_prio_t       prio;
    const char     *name;
    tb_task_fn      fn;
    void           *arg;
    tb_arg_dtor_fn  dtor;
    struct tb_incoming *next;
} tb_incoming_t;

struct tb_loop {
    int             epoll_fd;
    int             wakeup_fd;
    pthread_t       owner;
    bool            owner_set;
    pthread_mutex_t incoming_lock;
    tb_incoming_t  *incoming_head;   /* LIFO-стек */

    tb_pqueue_t     ready;           /* готовые задачи: приоритет + seq */
    tb_timerwheel_t wheel;
    tb_task_table_t active;

    struct tb_threadpool *pool;

    volatile bool  stop_requested;
    volatile bool  running;
    bool           reject_warned; /* атомарно: WARN отклонений — один раз */
    uint64_t       reject_count;  /* атомарно: отклонённые постинги */

    uint64_t        next_task_id;
    uint64_t        next_seq;
    tb_loop_stats_t stats;

    struct {
        int      fd;
        uint32_t events;
        tb_io_cb cb;
        void    *arg;
        bool     used;
    } watches[TB_LOOP_MAX_WATCHES];
};

/* ---- Внутренние ---- */

static void loop_wakeup(tb_loop_t *loop)
{
    uint64_t one = 1;
    ssize_t rc = write(loop->wakeup_fd, &one, sizeof(one));
    (void)rc; /* EAGAIN при переполнении счётчика — пробуждение уже случилось */
}

static void reap_task(tb_loop_t *loop, struct tb_task *t)
{
    tb_task_table_remove(&loop->active, t);
    tb_task_free(t);
}

/* Только из потока цикла. */
static uint64_t spawn_task(tb_loop_t *loop, tb_prio_t prio, const char *name,
                           tb_task_fn fn, void *arg, tb_arg_dtor_fn dtor)
{
    struct tb_task *t = tb_task_new(++loop->next_task_id, prio, name, fn, arg, dtor);
    if (!t) {
        TB_LOG_ERROR("loop: нет памяти на задачу '%s'", name ? name : "?");
        return 0;
    }
    if (!tb_task_table_add(&loop->active, t)) {
        tb_task_free(t);
        TB_LOG_ERROR("loop: таблица задач переполнена ('%s')", name ? name : "?");
        return 0;
    }
    t->seq = loop->next_seq++;
    loop->stats.tasks_posted++;
    if (!tb_pqueue_push(&loop->ready, t)) {
        tb_task_table_remove(&loop->active, t);
        tb_task_free(t);
        TB_LOG_ERROR("loop: ready-очередь не приняла задачу '%s'", name ? name : "?");
        return 0;
    }
    t->state = TB_TASK_QUEUED;
    t->in_heap = true;
    return t->id;
}

static void drain_incoming(tb_loop_t *loop)
{
    tb_incoming_t *list;
    pthread_mutex_lock(&loop->incoming_lock);
    list = loop->incoming_head;
    loop->incoming_head = NULL;
    pthread_mutex_unlock(&loop->incoming_lock);

    while (list) {
        tb_incoming_t *next = list->next;
        spawn_task(loop, list->prio, list->name, list->fn, list->arg, list->dtor);
        free(list);
        list = next;
    }
}

/* Очистка без исполнения: для уничтожения цикла. */
static void purge_incoming(tb_loop_t *loop)
{
    tb_incoming_t *list;
    pthread_mutex_lock(&loop->incoming_lock);
    list = loop->incoming_head;
    loop->incoming_head = NULL;
    pthread_mutex_unlock(&loop->incoming_lock);

    while (list) {
        tb_incoming_t *next = list->next;
        if (list->dtor)
            list->dtor(list->arg);
        TB_LOG_WARN("loop: постинг '%s' сброшен — цикл остановлен",
                    list->name ? list->name : "?");
        free(list);
        list = next;
    }
}

static void run_ready_batch(tb_loop_t *loop)
{
    while (!tb_pqueue_empty(&loop->ready)) {
        struct tb_task *t = tb_pqueue_pop(&loop->ready);
        t->in_heap = false;

        if (tb_atomic_load_bool(&t->canceled)) {
            reap_task(loop, t);
            loop->stats.tasks_canceled++;
            continue;
        }

        t->state = TB_TASK_RUNNING;
        t->fn(loop, t, t->arg);
        loop->stats.tasks_run++;

        if (t->period_ms && !tb_atomic_load_bool(&t->canceled)) {
            /* Периодическая: перевзвод. */
            t->deadline_ms = tb_now_ms() + t->period_ms;
            t->state = TB_TASK_PENDING;
            tb_timerwheel_add(&loop->wheel, t);
        } else {
            t->state = tb_atomic_load_bool(&t->canceled) ? TB_TASK_CANCELED
                       : (t->failed ? TB_TASK_FAILED : TB_TASK_DONE);
            reap_task(loop, t);
        }

        if (tb_atomic_load_bool(&loop->stop_requested))
            break;
    }
}

/* ---- Публичные ---- */

tb_loop_t *tb_loop_create(int pool_threads, tb_error_t *err)
{
    tb_loop_t *loop = calloc(1, sizeof(tb_loop_t));
    if (!loop) {
        TB_ERROR_SET(err, TB_DOMAIN_CORE, TB_CORE_NO_MEMORY,
                     "loop: нет памяти");
        return NULL;
    }
    loop->epoll_fd = -1;
    loop->wakeup_fd = -1;

    loop->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    loop->wakeup_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (loop->epoll_fd < 0 || loop->wakeup_fd < 0) {
        TB_ERROR_SET(err, TB_DOMAIN_SCHED, TB_CORE_IO,
                     "loop: epoll/eventfd недоступны");
        tb_loop_destroy(loop);
        return NULL;
    }

    struct epoll_event ev = { .events = EPOLLIN, .data.u32 = 0 };
    if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_ADD, loop->wakeup_fd, &ev) != 0) {
        TB_ERROR_SET(err, TB_DOMAIN_SCHED, TB_CORE_IO,
                     "loop: не удалось зарегистрировать wakeup fd");
        tb_loop_destroy(loop);
        return NULL;
    }

    if (!tb_pqueue_init(&loop->ready, tb_task_cmp_ready, 256) ||
        !tb_timerwheel_init(&loop->wheel) ||
        !tb_task_table_init(&loop->active, 12 /* 4096 задач */)) {
        TB_ERROR_SET(err, TB_DOMAIN_CORE, TB_CORE_NO_MEMORY,
                     "loop: инициализация контейнеров");
        tb_loop_destroy(loop);
        return NULL;
    }

    pthread_mutex_init(&loop->incoming_lock, NULL);

    /* Создавший поток — будущий владелец цикла: ему разрешено планировать
     * задачи до запуска (setup-фаза). */
    loop->owner = pthread_self();
    loop->owner_set = true;

    if (pool_threads > 0) {
        loop->pool = tb_threadpool_create(pool_threads, err);
        if (!loop->pool) {
            tb_loop_destroy(loop);
            return NULL;
        }
    }
    return loop;
}

void tb_loop_destroy(tb_loop_t *loop)
{
    if (!loop) return;
    if (loop->running) {
        TB_LOG_WARN("loop: уничтожение работающего цикла — останавливаю");
        tb_loop_stop(loop);
    }
    if (loop->pool)
        tb_threadpool_destroy(loop->pool);
    purge_incoming(loop);
    if (loop->epoll_fd >= 0) close(loop->epoll_fd);
    if (loop->wakeup_fd >= 0) close(loop->wakeup_fd);
    /* Таблица владеет всеми задачами: готовые, колесные, periodic. */
    tb_task_table_destroy(&loop->active);
    tb_pqueue_destroy(&loop->ready);
    tb_timerwheel_destroy(&loop->wheel);
    pthread_mutex_destroy(&loop->incoming_lock);
    { /* после join: воркеры дорабатывали блокирующие вызовы — счётчик
       * отклонённых результатов окончателен только здесь */
        uint64_t rej = __atomic_load_n(&loop->reject_count,
                                       __ATOMIC_RELAXED);
        if (rej)
            TB_LOG_WARN("loop: отклонено постингов в остановленный "
                        "цикл: %llu (воркеры дорабатывали блокирующие "
                        "вызовы)", (unsigned long long)rej);
    }
    free(loop);
}

int tb_loop_run(tb_loop_t *loop)
{
    if (!loop) return -1;
    if (loop->running) {
        TB_LOG_ERROR("loop: повторный запуск активного цикла отклонён");
        return -1;
    }
    if (!pthread_equal(loop->owner, pthread_self())) {
        TB_LOG_ERROR("loop: запуск возможен только из потока-создателя");
        return -1;
    }
    tb_atomic_store_bool(&loop->running, true);
    /* stop_requested НЕ сбрасываем: сигнал остановки мог прийти после
     * создания цикла, но ДО входа в run (стартовые проверки идут
     * долго) — прежний сброс превращал его в вечный цикл (дедлок
     * пойман stop-storm тестом H1.3). Начальное значение false —
     * штатный запуск не отличается; остановленный до старта цикл
     * выходит немедленно */

    int rc = 0;
    while (!tb_atomic_load_bool(&loop->stop_requested)) {
        uint64_t now = tb_now_ms();
        tb_timerwheel_advance(&loop->wheel, now, &loop->ready);
        loop->stats.timers_fired  = loop->wheel.fired;
        loop->stats.wheel_cascades = loop->wheel.cascaded;
        drain_incoming(loop);

        if (!tb_pqueue_empty(&loop->ready)) {
            run_ready_batch(loop);
            continue; /* свежие постинги и таймеры — без epoll */
        }

        int timeout_ms = 1000; /* потолок ожидания: защита от дрейфа часов */
        uint64_t dl;
        if (tb_timerwheel_next_deadline(&loop->wheel, &dl)) {
            if (dl <= now) timeout_ms = 0;
            else if (dl - now < 1000) timeout_ms = (int)(dl - now);
        }

        struct epoll_event evs[8];
        int n = epoll_wait(loop->epoll_fd, evs, 8, timeout_ms);
        loop->stats.polls++;
        if (n < 0) {
            if (errno == EINTR) continue;
            TB_LOG_ERROR("loop: epoll_wait: %s", strerror(errno));
            rc = -1;
            break;
        }
        for (int i = 0; i < n; i++) {
            uint32_t id = evs[i].data.u32;
            if (id == 0) { /* wakeup */
                uint64_t val;
                while (read(loop->wakeup_fd, &val, sizeof(val)) == (ssize_t)sizeof(val))
                    ;
                loop->stats.wakeups++;
                continue;
            }
            uint32_t wi = id - 1;
            if (wi < TB_LOOP_MAX_WATCHES && loop->watches[wi].used) {
                loop->stats.io_events++;
                loop->watches[wi].cb(loop, loop->watches[wi].fd,
                                     evs[i].events, loop->watches[wi].arg);
            }
        }
    }

    tb_atomic_store_bool(&loop->running, false);
    return rc;
}

void tb_loop_stop(tb_loop_t *loop)
{
    if (!loop) return;
    tb_atomic_store_bool(&loop->stop_requested, true);
    loop_wakeup(loop); /* write() асинхронно-сигнально-безопасен */
}

bool tb_loop_is_owner(const tb_loop_t *loop)
{
    return loop && loop->owner_set && pthread_equal(pthread_self(), loop->owner);
}

uint64_t tb_loop_post(tb_loop_t *loop, tb_prio_t prio, const char *name,
                      tb_task_fn fn, void *arg, tb_arg_dtor_fn dtor)
{
    if (!loop || !fn) return 0;

    if (loop->owner_set && pthread_equal(pthread_self(), loop->owner))
        return spawn_task(loop, prio, name, fn, arg, dtor);

    /* Цикл уже остановлен и не будет дренировать — честно отказываем.
     * На остановке дорабатывающие воркеры сыплются пачкой: логируем
     * ПЕРВОЕ отклонение и считаем остальные (итог — в destroy). */
    if (loop->owner_set && !tb_atomic_load_bool(&loop->running)) {
        if (dtor) dtor(arg);
        if (!__atomic_test_and_set(&loop->reject_warned,
                                  __ATOMIC_RELAXED)) {
            TB_LOG_WARN("loop: цикл остановлен — результаты воркеров "
                        "отклоняются (счётчик в destroy)");
        }
        __atomic_fetch_add(&loop->reject_count, 1, __ATOMIC_RELAXED);
        return 0;
    }

    tb_incoming_t *inc = malloc(sizeof(*inc));
    if (!inc) {
        if (dtor) dtor(arg);
        return 0;
    }
    inc->prio = prio;
    inc->name = name;
    inc->fn   = fn;
    inc->arg  = arg;
    inc->dtor = dtor;
    inc->next = NULL;

    pthread_mutex_lock(&loop->incoming_lock);
    inc->next = loop->incoming_head;
    loop->incoming_head = inc;
    pthread_mutex_unlock(&loop->incoming_lock);

    loop_wakeup(loop);
    /* id назначается при дрейне в потоке цикла; отмена кросс-поточных
     * постингов — через задачи-токены владельца. */
    return 0;
}

uint64_t tb_loop_call_at(tb_loop_t *loop, uint64_t at_ms, tb_prio_t prio,
                         const char *name, tb_task_fn fn, void *arg,
                         tb_arg_dtor_fn dtor)
{
    if (!loop || !fn) return 0;
    if (!tb_loop_is_owner(loop)) {
        TB_LOG_WARN("loop: call_at('%s') вне потока цикла отклонён", name ? name : "?");
        if (dtor) dtor(arg);
        return 0;
    }
    struct tb_task *t = tb_task_new(++loop->next_task_id, prio, name, fn, arg, dtor);
    if (!t) return 0;
    if (!tb_task_table_add(&loop->active, t)) {
        tb_task_free(t);
        return 0;
    }
    t->seq = loop->next_seq++;
    t->deadline_ms = at_ms;
    loop->stats.tasks_posted++;

    if (at_ms <= tb_now_ms()) {
        if (!tb_pqueue_push(&loop->ready, t)) {
            tb_task_table_remove(&loop->active, t);
            tb_task_free(t);
            return 0;
        }
        t->state = TB_TASK_QUEUED;
        t->in_heap = true;
    } else {
        tb_timerwheel_add(&loop->wheel, t);
    }
    return t->id;
}

uint64_t tb_loop_call_later(tb_loop_t *loop, uint64_t delay_ms, tb_prio_t prio,
                            const char *name, tb_task_fn fn, void *arg,
                            tb_arg_dtor_fn dtor)
{
    return tb_loop_call_at(loop, tb_now_ms() + delay_ms, prio, name, fn, arg, dtor);
}

uint64_t tb_loop_call_every(tb_loop_t *loop, uint64_t period_ms, tb_prio_t prio,
                            const char *name, tb_task_fn fn, void *arg,
                            tb_arg_dtor_fn dtor)
{
    uint64_t id = tb_loop_call_at(loop, tb_now_ms() + period_ms, prio, name,
                                  fn, arg, dtor);
    if (id) {
        struct tb_task *t = tb_task_table_find(&loop->active, id);
        if (t) t->period_ms = period_ms;
    }
    return id;
}

bool tb_loop_cancel(tb_loop_t *loop, uint64_t task_id)
{
    if (!loop || task_id == 0) return false;
    if (!tb_loop_is_owner(loop)) {
        TB_LOG_WARN("loop: cancel вне потока цикла отклонён (id=%llu)",
                    (unsigned long long)task_id);
        return false;
    }
    struct tb_task *t = tb_task_table_find(&loop->active, task_id);
    if (!t) return false;
    tb_atomic_store_bool(&t->canceled, true);
    return true;
}

struct tb_task *tb_loop_find_task(tb_loop_t *loop, uint64_t task_id)
{
    if (!loop || !tb_loop_is_owner(loop)) return NULL;
    return tb_task_table_find(&loop->active, task_id);
}

int tb_loop_io_watch(tb_loop_t *loop, int fd, uint32_t epoll_events,
                     tb_io_cb cb, void *arg)
{
    if (!loop || !cb || fd < 0) return -1;
    if (!tb_loop_is_owner(loop)) {
        TB_LOG_WARN("loop: io_watch вне потока цикла отклонён");
        return -1;
    }
    for (int i = 0; i < TB_LOOP_MAX_WATCHES; i++) {
        if (loop->watches[i].used) continue;
        struct epoll_event ev = { .events = epoll_events, .data.u32 = (uint32_t)(i + 1) };
        if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_ADD, fd, &ev) != 0) {
            TB_LOG_ERROR("loop: epoll_ctl ADD: %s", strerror(errno));
            return -1;
        }
        loop->watches[i].fd = fd;
        loop->watches[i].events = epoll_events;
        loop->watches[i].cb = cb;
        loop->watches[i].arg = arg;
        loop->watches[i].used = true;
        return i;
    }
    TB_LOG_ERROR("loop: лимит наблюдений fd исчерпан (%d)", TB_LOOP_MAX_WATCHES);
    return -1;
}

bool tb_loop_io_unwatch(tb_loop_t *loop, int watch_id)
{
    if (!loop || watch_id < 0 || watch_id >= TB_LOOP_MAX_WATCHES) return false;
    if (!loop->watches[watch_id].used) return false;
    epoll_ctl(loop->epoll_fd, EPOLL_CTL_DEL, loop->watches[watch_id].fd, NULL);
    memset(&loop->watches[watch_id], 0, sizeof(loop->watches[watch_id]));
    return true;
}

struct tb_threadpool *tb_loop_pool(tb_loop_t *loop) { return loop ? loop->pool : NULL; }
const tb_loop_stats_t *tb_loop_stats(const tb_loop_t *loop) { return loop ? &loop->stats : NULL; }
