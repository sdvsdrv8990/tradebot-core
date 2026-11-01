/* ==========================================================================
 *  tb/sched/loop.h — событийный цикл (гибридная модель).
 *
 *  Один поток-владелец выполняет все callback'и: таймеры, ready-задачи,
 *  события epoll. Блокирующая работа (HTTP-запросы к источникам, запись
 *  архива) выносится в пул потоков, а завершение возвращается в цикл
 *  через tb_loop_post() — типичная модель nginx/redis + worker pool.
 *
 *  Пробуждение из других потоков — через eventfd, поэтому кросс-поточные
 *  постинги дешёвые и не требуют захватывать блокировки цикла надолго.
 * ========================================================================== */
#ifndef TB_SCHED_LOOP_H
#define TB_SCHED_LOOP_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include "tb/sched/task.h"
#include "tb/sched/timer.h"

#ifdef __cplusplus
extern "C" {
#endif

struct tb_threadpool;

#define TB_LOOP_MAX_WATCHES 64

typedef struct tb_loop tb_loop_t;

/* Событие готовности fd (флаги epoll). */
typedef void (*tb_io_cb)(tb_loop_t *loop, int fd, uint32_t events, void *arg);

typedef struct tb_loop_stats {
    uint64_t tasks_posted;
    uint64_t tasks_run;
    uint64_t tasks_canceled;
    uint64_t timers_fired;
    uint64_t wheel_cascades;
    uint64_t wakeups;         /* пробуждения eventfd от других потоков */
    uint64_t io_events;       /* сработавшие наблюдения fd */
    uint64_t polls;           /* итерации epoll_wait */
} tb_loop_stats_t;

/* pool_threads: число воркеров пула блокирующих операций; 0 — без пула. */
tb_loop_t *tb_loop_create(int pool_threads, tb_error_t *err);
void       tb_loop_destroy(tb_loop_t *loop);

/* Запуск в текущем потоке. Возврат — по tb_loop_stop() или ошибке цикла.
 * Повторный запуск допустим после возврата. */
int tb_loop_run(tb_loop_t *loop);

/* Потокобезопасная остановка (асинхронно-сигнально-безопасная). */
void tb_loop_stop(tb_loop_t *loop);

/* Текущий поток — владелец цикла. */
bool tb_loop_is_owner(const tb_loop_t *loop);

/* ---- Постинг задач ----
 * post        — выполнить как можно скорее;
 * call_at     — в абсолютный монотонный момент;
 * call_later  — через delay_ms;
 * call_every  — периодически (первый запуск через period_ms).
 * Возвращают id задачи (0 — ошибка). name — статическая строка. */
uint64_t tb_loop_post(tb_loop_t *loop, tb_prio_t prio, const char *name,
                      tb_task_fn fn, void *arg, tb_arg_dtor_fn dtor);
uint64_t tb_loop_call_at(tb_loop_t *loop, uint64_t at_ms, tb_prio_t prio,
                         const char *name, tb_task_fn fn, void *arg,
                         tb_arg_dtor_fn dtor);
uint64_t tb_loop_call_later(tb_loop_t *loop, uint64_t delay_ms, tb_prio_t prio,
                            const char *name, tb_task_fn fn, void *arg,
                            tb_arg_dtor_fn dtor);
uint64_t tb_loop_call_every(tb_loop_t *loop, uint64_t period_ms, tb_prio_t prio,
                            const char *name, tb_task_fn fn, void *arg,
                            tb_arg_dtor_fn dtor);

/* Отмена по id. Вызывать из потока цикла (из других потоков — постингом
 * задачи-отмены). Физическое освобождение — при выборке из контейнеров. */
bool tb_loop_cancel(tb_loop_t *loop, uint64_t task_id);

/* Задача по id (только из потока цикла). */
struct tb_task *tb_loop_find_task(tb_loop_t *loop, uint64_t task_id);

/* ---- Наблюдение fd (epoll) ----
 * Возвращает watch_id >= 0 или -1. Модификация — только из потока цикла. */
int  tb_loop_io_watch(tb_loop_t *loop, int fd, uint32_t epoll_events,
                      tb_io_cb cb, void *arg);
bool tb_loop_io_unwatch(tb_loop_t *loop, int watch_id);

/* ---- Доступ к подсистемам цикла ---- */
struct tb_threadpool *tb_loop_pool(tb_loop_t *loop);
const tb_loop_stats_t *tb_loop_stats(const tb_loop_t *loop);

#ifdef __cplusplus
}
#endif
#endif /* TB_SCHED_LOOP_H */
