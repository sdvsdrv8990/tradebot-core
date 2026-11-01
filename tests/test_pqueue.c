/* tests/test_pqueue.c — приоритетная очередь задач. */
#include "harness.h"
#include "tb/sched/pqueue.h"

static struct tb_task *mk(uint64_t id, tb_prio_t prio, uint64_t seq, uint64_t dl)
{
    struct tb_task *t = tb_task_new(id, prio, "t", NULL, NULL, NULL);
    CHECK(t != NULL);
    t->seq = seq;
    t->deadline_ms = dl;
    return t;
}

int main(void)
{
    /* ---- порядок: приоритет, затем seq ---- */
    tb_pqueue_t q;
    CHECK(tb_pqueue_init(&q, tb_task_cmp_ready, 4));

    struct tb_task *a = mk(1, TB_PRIO_NORMAL, 1, 0);
    struct tb_task *b = mk(2, TB_PRIO_BACKGROUND, 2, 0);
    struct tb_task *c = mk(3, TB_PRIO_CRITICAL, 3, 0);
    struct tb_task *d = mk(4, TB_PRIO_NORMAL, 4, 0);

    CHECK(tb_pqueue_push(&q, a));
    CHECK(tb_pqueue_push(&q, b));
    CHECK(tb_pqueue_push(&q, c));
    CHECK(tb_pqueue_push(&q, d));
    CHECK(tb_pqueue_size(&q) == 4);

    CHECK(tb_pqueue_pop(&q) == c);  /* CRITICAL */
    CHECK(tb_pqueue_pop(&q) == a);  /* NORMAL seq 1 */
    CHECK(tb_pqueue_pop(&q) == d);  /* NORMAL seq 4 */
    CHECK(tb_pqueue_pop(&q) == b);  /* BACKGROUND */
    CHECK(tb_pqueue_empty(&q));
    CHECK(tb_pqueue_pop(&q) == NULL);

    /* ---- рост кучи ---- */
    for (int i = 0; i < 1000; i++) {
        struct tb_task *t = mk(100 + (uint64_t)i,
                               (tb_prio_t)(i % 5), (uint64_t)i, 0);
        CHECK(tb_pqueue_push(&q, t));
    }
    CHECK(tb_pqueue_size(&q) == 1000);
    tb_prio_t prev = TB_PRIO_CRITICAL;
    uint64_t prev_seq = 0;
    for (int i = 0; i < 1000; i++) {
        struct tb_task *t = tb_pqueue_pop(&q);
        CHECK(t != NULL);
        CHECK(t->prio >= prev);
        if (t->prio == prev)
            CHECK(t->seq >= prev_seq);
        prev = t->prio;
        prev_seq = t->seq;
        tb_task_free(t);
    }
    tb_pqueue_destroy(&q);

    /* ---- режим дедлайнов ---- */
    CHECK(tb_pqueue_init(&q, tb_task_cmp_deadline, 8));
    struct tb_task *t50 = mk(1, TB_PRIO_NORMAL, 1, 50);
    struct tb_task *t10 = mk(2, TB_PRIO_BACKGROUND, 2, 10);
    struct tb_task *t99 = mk(3, TB_PRIO_CRITICAL, 3, 99);
    tb_pqueue_push(&q, t50);
    tb_pqueue_push(&q, t10);
    tb_pqueue_push(&q, t99);
    CHECK(tb_pqueue_pop(&q) == t10);
    CHECK(tb_pqueue_pop(&q) == t50);
    CHECK(tb_pqueue_pop(&q) == t99);
    tb_task_free(t10);
    tb_task_free(t50);
    tb_task_free(t99);
    tb_task_free(a); tb_task_free(b); tb_task_free(c); tb_task_free(d);
    tb_pqueue_destroy(&q);

    printf("OK test_pqueue\n");
    return 0;
}
