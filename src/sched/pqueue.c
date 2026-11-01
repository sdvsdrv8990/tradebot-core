/* ==========================================================================
 *  tb/sched/pqueue.c — бинарная куча поверх tb_task_t*.
 * ========================================================================== */
#include "tb/sched/pqueue.h"

#include <stdlib.h>
#include <string.h>

bool tb_pqueue_init(tb_pqueue_t *q, tb_task_cmp_fn cmp, size_t cap)
{
    if (!q || !cmp || cap == 0) return false;
    q->heap = calloc(cap, sizeof(struct tb_task *));
    if (!q->heap) return false;
    q->cap  = cap;
    q->size = 0;
    q->cmp  = cmp;
    return true;
}

void tb_pqueue_destroy(tb_pqueue_t *q)
{
    if (!q) return;
    free(q->heap);
    memset(q, 0, sizeof(*q));
}

static void heap_swap(tb_pqueue_t *q, size_t i, size_t j)
{
    struct tb_task *tmp = q->heap[i];
    q->heap[i] = q->heap[j];
    q->heap[j] = tmp;
    q->heap[i]->heap_idx = i;
    q->heap[j]->heap_idx = j;
}

static void sift_up(tb_pqueue_t *q, size_t i)
{
    while (i > 0) {
        size_t parent = (i - 1) / 2;
        if (q->cmp(q->heap[i], q->heap[parent]) < 0) {
            heap_swap(q, i, parent);
            i = parent;
        } else {
            break;
        }
    }
}

static void sift_down(tb_pqueue_t *q, size_t i)
{
    for (;;) {
        size_t l = 2 * i + 1, r = 2 * i + 2, best = i;
        if (l < q->size && q->cmp(q->heap[l], q->heap[best]) < 0) best = l;
        if (r < q->size && q->cmp(q->heap[r], q->heap[best]) < 0) best = r;
        if (best == i) break;
        heap_swap(q, i, best);
        i = best;
    }
}

bool tb_pqueue_push(tb_pqueue_t *q, struct tb_task *t)
{
    if (!q || !t) return false;
    if (q->size == q->cap) {
        size_t ncap = q->cap * 2;
        struct tb_task **nheap = realloc(q->heap, ncap * sizeof(struct tb_task *));
        if (!nheap) return false;
        q->heap = nheap;
        q->cap  = ncap;
    }
    q->heap[q->size] = t;
    t->heap_idx = q->size;
    q->size++;
    sift_up(q, q->size - 1);
    return true;
}

struct tb_task *tb_pqueue_pop(tb_pqueue_t *q)
{
    if (!q || q->size == 0) return NULL;
    struct tb_task *top = q->heap[0];
    top->heap_idx = (size_t)-1;
    q->size--;
    if (q->size > 0) {
        q->heap[0] = q->heap[q->size];
        q->heap[0]->heap_idx = 0;
        sift_down(q, 0);
    }
    return top;
}

struct tb_task *tb_pqueue_peek(const tb_pqueue_t *q)
{
    if (!q || q->size == 0) return NULL;
    return q->heap[0];
}

bool tb_pqueue_empty(const tb_pqueue_t *q)
{
    return !q || q->size == 0;
}

size_t tb_pqueue_size(const tb_pqueue_t *q)
{
    return q ? q->size : 0;
}
