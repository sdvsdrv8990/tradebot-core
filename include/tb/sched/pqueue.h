/* ==========================================================================
 *  tb/sched/pqueue.h — бинарная куча задач.
 *
 *  Инвариант проекта: задача может находиться максимум в одной куче
 *  единовременно (ready-очередь цикла ИЛИ overflow-куча таймерного
 *  колеса), поэтому t->heap_idx корректен для той кучи, где лежит задача.
 * ========================================================================== */
#ifndef TB_SCHED_PQUEUE_H
#define TB_SCHED_PQUEUE_H

#include <stdbool.h>
#include <stddef.h>
#include "tb/sched/task.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*tb_task_cmp_fn)(const struct tb_task *a, const struct tb_task *b);

typedef struct tb_pqueue {
    struct tb_task **heap;
    size_t            cap;
    size_t            size;
    tb_task_cmp_fn    cmp;
} tb_pqueue_t;

bool tb_pqueue_init(tb_pqueue_t *q, tb_task_cmp_fn cmp, size_t cap);
void tb_pqueue_destroy(tb_pqueue_t *q);

bool             tb_pqueue_push(tb_pqueue_t *q, struct tb_task *t);
struct tb_task  *tb_pqueue_pop(tb_pqueue_t *q);
struct tb_task  *tb_pqueue_peek(const tb_pqueue_t *q);

bool   tb_pqueue_empty(const tb_pqueue_t *q);
size_t tb_pqueue_size(const tb_pqueue_t *q);

#ifdef __cplusplus
}
#endif
#endif /* TB_SCHED_PQUEUE_H */
