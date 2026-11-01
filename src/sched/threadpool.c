#define TB_CURRENT_GROUP TB_G_POOL /* зона ответственности файла */
/* ==========================================================================
 *  tb/sched/threadpool.c — пул воркеров блокирующих операций.
 * ========================================================================== */
#include "tb/sched/threadpool.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "tb/core/log.h"
#include "tb/sched/loop.h"

typedef struct tb_pool_job {
    tb_pool_submit_t s;
    struct tb_pool_job *next;
} tb_pool_job_t;

struct tb_threadpool {
    pthread_t      *threads;
    size_t          nthreads;
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    tb_pool_job_t  *head, *tail;   /* FIFO */
    size_t          pending;
    bool            closing;
    uint64_t        completed;
};

/* Завершение пула -> цикл: arg = tb_completion_t. */
typedef struct {
    tb_job_done_fn  done;
    void           *arg;
    tb_arg_dtor_fn  arg_dtor;
} tb_completion_t;

static void completion_run(tb_loop_t *loop, struct tb_task *task, void *arg)
{
    tb_completion_t *c = arg;
    (void)task;
    if (c->done)
        c->done(loop, c->arg);
    /* arg и сама структура освобождаются ТОЛЬКО dtor задачи
     * (completion_dtor) — единственный владелец жизненного цикла. */
}

static void completion_dtor(void *arg)
{
    tb_completion_t *c = arg;
    if (c->arg_dtor)
        c->arg_dtor(c->arg);
    free(c);
}

static void post_completion(const tb_pool_submit_t *s)
{
    tb_completion_t *c = malloc(sizeof(*c));
    if (!c) {
        if (s->arg_dtor) s->arg_dtor(s->arg);
        TB_LOG_ERROR("pool: нет памяти на completion — результат потерян");
        return;
    }
    c->done     = s->done;
    c->arg      = s->arg;
    c->arg_dtor = s->arg_dtor;
    tb_loop_post(s->loop, s->done_prio, "pool.done",
                 completion_run, c, completion_dtor);
}

static void *worker_main(void *p)
{
    tb_threadpool_t *pool = p;
    for (;;) {
        pthread_mutex_lock(&pool->lock);
        while (!pool->head && !pool->closing)
            pthread_cond_wait(&pool->cond, &pool->lock);
        if (!pool->head && pool->closing) {
            pthread_mutex_unlock(&pool->lock);
            return NULL;
        }
        tb_pool_job_t *j = pool->head;
        pool->head = j->next;
        if (!pool->head) pool->tail = NULL;
        pool->pending--;
        pthread_mutex_unlock(&pool->lock);

        j->s.job(j->s.arg); /* блокирующая работа */

        if (j->s.loop && j->s.done)
            post_completion(&j->s);
        else if (j->s.arg_dtor)
            j->s.arg_dtor(j->s.arg);

        free(j);
        __atomic_fetch_add(&pool->completed, 1, __ATOMIC_RELAXED);
    }
}

tb_threadpool_t *tb_threadpool_create(int threads, tb_error_t *err)
{
    if (threads <= 0) {
        TB_ERROR_SET(err, TB_DOMAIN_SCHED, TB_CORE_INVALID_ARG,
                     "pool: threads > 0 обязателен");
        return NULL;
    }
    tb_threadpool_t *pool = calloc(1, sizeof(tb_threadpool_t));
    if (!pool) {
        TB_ERROR_SET(err, TB_DOMAIN_CORE, TB_CORE_NO_MEMORY, "pool: нет памяти");
        return NULL;
    }
    pool->threads = calloc((size_t)threads, sizeof(pthread_t));
    if (!pool->threads) {
        free(pool);
        TB_ERROR_SET(err, TB_DOMAIN_CORE, TB_CORE_NO_MEMORY, "pool: нет памяти");
        return NULL;
    }
    pthread_mutex_init(&pool->lock, NULL);
    pthread_cond_init(&pool->cond, NULL);

    for (int i = 0; i < threads; i++) {
        if (pthread_create(&pool->threads[i], NULL, worker_main, pool) != 0) {
            pool->nthreads = (size_t)i;
            tb_threadpool_destroy(pool);
            TB_ERROR_SET(err, TB_DOMAIN_SCHED, TB_CORE_IO,
                         "pool: не удалось создать поток %d", i);
            return NULL;
        }
        pool->nthreads = (size_t)(i + 1);
    }
    return pool;
}

void tb_threadpool_destroy(tb_threadpool_t *pool)
{
    if (!pool) return;
    pthread_mutex_lock(&pool->lock);
    pool->closing = true;
    pthread_cond_broadcast(&pool->cond);
    pthread_mutex_unlock(&pool->lock);

    for (size_t i = 0; i < pool->nthreads; i++)
        pthread_join(pool->threads[i], NULL);

    /* Оставшиеся в очереди задания не исполнялись — чистим через arg_dtor. */
    tb_pool_job_t *j = pool->head;
    while (j) {
        tb_pool_job_t *next = j->next;
        if (j->s.arg_dtor)
            j->s.arg_dtor(j->s.arg);
        free(j);
        j = next;
    }
    free(pool->threads);
    pthread_mutex_destroy(&pool->lock);
    pthread_cond_destroy(&pool->cond);
    free(pool);
}

bool tb_threadpool_submit(tb_threadpool_t *pool, const tb_pool_submit_t *s)
{
    if (!pool || !s || !s->job) return false;

    tb_pool_job_t *j = malloc(sizeof(*j));
    if (!j) return false;
    j->s = *s;
    j->next = NULL;

    pthread_mutex_lock(&pool->lock);
    if (pool->closing) {
        pthread_mutex_unlock(&pool->lock);
        free(j);
        return false;
    }
    if (pool->tail)
        pool->tail->next = j;
    else
        pool->head = j;
    pool->tail = j;
    pool->pending++;
    pthread_cond_signal(&pool->cond);
    pthread_mutex_unlock(&pool->lock);
    return true;
}

size_t tb_threadpool_threads(const tb_threadpool_t *pool)
{
    return pool ? pool->nthreads : 0;
}

size_t tb_threadpool_pending(const tb_threadpool_t *pool)
{
    if (!pool) return 0;
    pthread_mutex_lock((pthread_mutex_t *)&pool->lock);
    size_t n = pool->pending;
    pthread_mutex_unlock((pthread_mutex_t *)&pool->lock);
    return n;
}

uint64_t tb_threadpool_completed(const tb_threadpool_t *pool)
{
    return pool ? __atomic_load_n(&pool->completed, __ATOMIC_RELAXED) : 0;
}
