/* tests/test_loop.c — интеграционный тест цикла: приоритеты, таймеры,
 * периодические задачи, пул потоков, отмена, остановка. */
#include "harness.h"
#include <unistd.h>
#include "tb/core/time.h"
#include "tb/sched/loop.h"
#include "tb/sched/threadpool.h"

#define MAX_ORDER 64

typedef struct {
    char     order[MAX_ORDER];
    int      n;
    int      periodic_count;
    uint64_t periodic_id;
    uint64_t t_start;
    uint64_t t_late;
    bool     pool_done_ok;      /* done выполнен в потоке цикла */
    int      pool_results;
} ctx_t;

static void record(ctx_t *c, char tag)
{
    CHECK(c->n < MAX_ORDER - 1);
    c->order[c->n++] = tag;
}

static void task_a1(tb_loop_t *l, tb_task_t *t, void *arg) { (void)l;(void)t; record(arg, '1'); }
static void task_a2(tb_loop_t *l, tb_task_t *t, void *arg) { (void)l;(void)t; record(arg, '2'); }
static void task_a3(tb_loop_t *l, tb_task_t *t, void *arg) { (void)l;(void)t; record(arg, '3'); }
static void task_bg(tb_loop_t *l, tb_task_t *t, void *arg) { (void)l;(void)t; record(arg, 'b'); }
static void task_critical(tb_loop_t *l, tb_task_t *t, void *arg) { (void)l;(void)t; record(arg, 'c'); }

static void task_late(tb_loop_t *l, tb_task_t *t, void *arg)
{
    (void)l; (void)t;
    ctx_t *c = arg;
    c->t_late = tb_now_ms() - c->t_start;
    record(c, 'L');
}

static void task_periodic(tb_loop_t *l, tb_task_t *t, void *arg)
{
    (void)l;
    ctx_t *c = arg;
    c->periodic_count++;
    if (c->periodic_count == 3) {
        c->periodic_id = t->id;
        tb_loop_cancel(l, t->id); /* самозавершение периодической задачи */
    }
}

static void pool_job(void *arg)
{
    ctx_t *c = arg;
    tb_sleep_ms(30); /* блокирующая работа */
    CHECK(c != NULL);
}

static void pool_done(tb_loop_t *loop, void *arg)
{
    ctx_t *c = arg;
    c->pool_done_ok = tb_loop_is_owner(loop);
    c->pool_results++;
    record(c, 'P');
}

static void task_final(tb_loop_t *l, tb_task_t *t, void *arg)
{
    (void)t;
    ctx_t *c = arg;
    record(c, 'F');
    tb_loop_stop(l);
}

/* ---- Регрессия tb_task_table_remove: сдвиг-назад ----
 * Прежнее условие переноса ((j-i) >= (j-k)) переносило элементы через их
 * «дом»: цепочка пробирования рвалась, find/remove молча не находили
 * задачу, reap_task освобождал её при живом указателе в слоте (use-after-
 * free при destroy). Здесь гоняем таблицу разными раскладками и после
 * каждого удаления проверяем достижимость всех оставшихся. */
static void test_task_table_remove_shift(void)
{
    tb_task_table_t tbl;
    CHECK(tb_task_table_init(&tbl, 6)); /* 64 слота */
    enum { N = 40 };

    for (int round = 0; round < 24; round++) {
        uint64_t base = (uint64_t)round * 97 + 1;
        struct tb_task *tasks[N];

        for (int i = 0; i < N; i++) {
            tasks[i] = tb_task_new(base + (uint64_t)i, TB_PRIO_NORMAL,
                                   "tt", NULL, NULL, NULL);
            CHECK(tasks[i] != NULL);
            CHECK(tb_task_table_add(&tbl, tasks[i]));
        }

        /* Убираем каждый третий — дыры среди занятых слотов. remove не
         * владеет задачей: освободить обязан убравший (иначе leak —
         * ASan ловил 960 утёкших tb_task_new). */
        for (int i = 0; i < N; i += 3) {
            tb_task_table_remove(&tbl, tasks[i]);
            tb_task_free(tasks[i]);
            tasks[i] = NULL;
        }
        for (int i = 0; i < N; i++) {
            struct tb_task *f = tb_task_table_find(&tbl, base + (uint64_t)i);
            if (tasks[i]) CHECK(f == tasks[i]);
            else          CHECK(f == NULL);
        }

        /* Дополнительные дыры: каждый второй из оставшихся. */
        for (int i = 0; i < N; i += 2) {
            if (tasks[i]) {
                tb_task_table_remove(&tbl, tasks[i]);
                tb_task_free(tasks[i]);
                tasks[i] = NULL;
            }
        }
        for (int i = 0; i < N; i++) {
            struct tb_task *f = tb_task_table_find(&tbl, base + (uint64_t)i);
            if (tasks[i]) CHECK(f == tasks[i]);
            else          CHECK(f == NULL);
        }

        /* Полная очистка раунда. */
        for (int i = 0; i < N; i++)
            if (tasks[i]) {
                tb_task_table_remove(&tbl, tasks[i]);
                tb_task_free(tasks[i]);
            }
        CHECK(tb_task_table_count(&tbl) == 0);
    }
    tb_task_table_destroy(&tbl);
}

int main(void)
{
    test_task_table_remove_shift();

    tb_error_t err = TB_ERROR_INIT;
    tb_loop_t *loop = tb_loop_create(2, &err);
    CHECK(loop != NULL);
    CHECK(tb_loop_is_owner(loop)); /* создатель — владелец */

    static ctx_t c;
    memset(&c, 0, sizeof(c));
    c.t_start = tb_now_ms();

    /* Кросс-поточный постинг проверит пул ниже; здесь — приоритеты. */
    CHECK(tb_loop_post(loop, TB_PRIO_NORMAL, "a1", task_a1, &c, NULL) != 0);
    CHECK(tb_loop_post(loop, TB_PRIO_NORMAL, "a2", task_a2, &c, NULL) != 0);
    CHECK(tb_loop_post(loop, TB_PRIO_NORMAL, "a3", task_a3, &c, NULL) != 0);
    CHECK(tb_loop_post(loop, TB_PRIO_BACKGROUND, "bg", task_bg, &c, NULL) != 0);
    CHECK(tb_loop_post(loop, TB_PRIO_CRITICAL, "crit", task_critical, &c, NULL) != 0);

    /* Таймер ~120 мс. */
    CHECK(tb_loop_call_later(loop, 120, TB_PRIO_NORMAL, "late",
                             task_late, &c, NULL) != 0);

    /* Периодическая задача каждые 40 мс; сама отменится на 3-м запуске. */
    uint64_t pid = tb_loop_call_every(loop, 40, TB_PRIO_NORMAL, "per",
                                      task_periodic, &c, NULL);
    CHECK(pid != 0);

    /* Пул: блокирующая работа с продолжением в цикле. */
    tb_pool_submit_t sub = {
        .job = pool_job, .arg = &c, .arg_dtor = NULL,
        .loop = loop, .done_prio = TB_PRIO_HIGH, .done = pool_done,
    };
    CHECK(tb_threadpool_submit(tb_loop_pool(loop), &sub));

    /* Финальная проверка на 400 мс. */
    CHECK(tb_loop_call_later(loop, 400, TB_PRIO_CRITICAL, "final",
                             task_final, &c, NULL) != 0);

    CHECK(tb_loop_run(loop) == 0);

    /* ---- Проверки ---- */
    const tb_loop_stats_t *ls = tb_loop_stats(loop);

    /* Порядок первой партии: критичная впереди нормальных, фон — последним. */
    CHECK(c.n >= 7);
    CHECK(c.order[0] == 'c');
    CHECK(c.order[1] == '1' && c.order[2] == '2' && c.order[3] == '3');
    CHECK(c.order[4] == 'b');

    /* Таймер ~120 мс: допуск на гранулярность колеса и загрузку. */
    CHECK(c.t_late >= 115 && c.t_late <= 250);

    /* Периодическая: ровно 3 запуска, затем отменена. */
    CHECK(c.periodic_count == 3);

    /* Пул: done пришёл в поток цикла. */
    CHECK(c.pool_results == 1);
    CHECK(c.pool_done_ok == true);

    /* Все задачи исполнены; последняя — финальная. */
    CHECK(c.order[c.n - 1] == 'F');
    CHECK(ls->tasks_run == (uint64_t)c.n + (uint64_t)c.periodic_count);
    CHECK(ls->tasks_posted == (uint64_t)c.n + 1); /* + периодическая (её перевзвод не считается) */
    CHECK(ls->timers_fired >= 2);
    CHECK(tb_loop_find_task(loop, pid) == NULL); /* отменённая периодическая освобождена */

    CHECK(tb_threadpool_completed(tb_loop_pool(loop)) >= 1);

    tb_loop_stats_t ls_copy = *ls;
    tb_loop_destroy(loop);

    printf("OK test_loop (порядок=%.6s..., задач=%llu, таймеров=%llu)\n",
           c.order, (unsigned long long)ls_copy.tasks_run,
           (unsigned long long)ls_copy.timers_fired);
    return 0;
}
