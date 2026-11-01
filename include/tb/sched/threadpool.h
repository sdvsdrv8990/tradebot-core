/* ==========================================================================
 *  tb/sched/threadpool.h — пул потоков для блокирующих операций.
 *
 *  HTTP-запросы к источникам данных, запись исторического архива, fsync
 *  журнала — всё, что может «застыть», выполняется здесь. Завершение
 *  возвращается в поток цикла через done-callback, поэтому гонок по
 *  данным не возникает: результат готовится в job, публикуется в done.
 *
 *  Владение arg: передаётся пулу; arg_dtor вызывается ровно один раз —
 *  после done (или после job, если done нет, или при сбросе очереди).
 * ========================================================================== */
#ifndef TB_SCHED_THREADPOOL_H
#define TB_SCHED_THREADPOOL_H

#include <stdbool.h>
#include <stddef.h>
#include "tb/core/error.h"
#include "tb/sched/task.h"

#ifdef __cplusplus
extern "C" {
#endif

struct tb_loop;

typedef void (*tb_job_fn)(void *arg);                            /* поток пула */
typedef void (*tb_job_done_fn)(struct tb_loop *loop, void *arg); /* поток цикла */

typedef struct tb_pool_submit {
    tb_job_fn       job;        /* обязательное: блокирующая работа */
    void           *arg;        /* контекст, переживает job и done */
    tb_arg_dtor_fn  arg_dtor;   /* опционально; вызывается ровно один раз */
    struct tb_loop *loop;       /* для done; NULL — done не вызывать */
    tb_prio_t       done_prio;  /* приоритет постинга done в цикл */
    tb_job_done_fn  done;       /* опционально: продолжение в потоке цикла */
} tb_pool_submit_t;

typedef struct tb_threadpool tb_threadpool_t;

tb_threadpool_t *tb_threadpool_create(int threads, tb_error_t *err);

/* Дренирует очередь (без исполнения заданий, с arg_dtor), присоединяет
 * потоки. После возврата пул невалиден. */
void tb_threadpool_destroy(tb_threadpool_t *pool);

bool tb_threadpool_submit(tb_threadpool_t *pool, const tb_pool_submit_t *s);

size_t   tb_threadpool_threads(const tb_threadpool_t *pool);
size_t   tb_threadpool_pending(const tb_threadpool_t *pool);
uint64_t tb_threadpool_completed(const tb_threadpool_t *pool);

#ifdef __cplusplus
}
#endif
#endif /* TB_SCHED_THREADPOOL_H */
