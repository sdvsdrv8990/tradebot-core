/* tests/test_timer.c — таймерное колесо на виртуальном времени.
 *
 * Инвариант гранулярности: задача срабатывает на первом тике,
 * момент которого (кратен SLOT_MS) >= дедлайна.
 */
#include "harness.h"
#include "tb/sched/timer.h"

static struct tb_task *mk(uint64_t id, uint64_t deadline)
{
    struct tb_task *t = tb_task_new(id, TB_PRIO_NORMAL, "t", NULL, NULL, NULL);
    CHECK(t != NULL);
    t->seq = id;
    t->deadline_ms = deadline;
    return t;
}

static struct tb_task *pop_expect(tb_pqueue_t *ready, uint64_t id)
{
    struct tb_task *t = tb_pqueue_pop(ready);
    CHECK(t != NULL && t->id == id);
    return t;
}

int main(void)
{
    tb_timerwheel_t w;
    CHECK(tb_timerwheel_init(&w));
    tb_pqueue_t ready;
    CHECK(tb_pqueue_init(&ready, tb_task_cmp_ready, 16));

    /* Дедлайны: 40, 40(крит), 100, 20000 (overflow), 60000 (глубокий overflow). */
    struct tb_task *t100   = mk(1, 100);
    struct tb_task *t40a   = mk(2, 40);
    struct tb_task *t40c   = mk(3, 40);  t40c->prio = TB_PRIO_CRITICAL;
    struct tb_task *t20k   = mk(4, 20000);
    struct tb_task *t60k   = mk(5, 60000);

    tb_timerwheel_add(&w, t100);
    tb_timerwheel_add(&w, t40a);
    tb_timerwheel_add(&w, t40c);
    tb_timerwheel_add(&w, t20k);
    tb_timerwheel_add(&w, t60k);
    CHECK(w.pending == 5);

    /* Ближайший дедлайн — ближайший тик колеса. */
    uint64_t dl;
    CHECK(tb_timerwheel_next_deadline(&w, &dl));
    CHECK(dl == TB_TW_SLOT_MS);

    /* До 39: ничего. */
    tb_timerwheel_advance(&w, 39, &ready);
    CHECK(tb_pqueue_empty(&ready));
    CHECK(w.pending == 5);

    /* До 40: обе задачи слота 5; критичная — первой в ready. */
    tb_timerwheel_advance(&w, 40, &ready);
    CHECK(tb_pqueue_size(&ready) == 2);
    pop_expect(&ready, 3); /* CRITICAL */
    pop_expect(&ready, 2);
    CHECK(w.pending == 3);

    /* До 100: слот задачи — 104 (ceil(100/8)=13), ещё не время. */
    tb_timerwheel_advance(&w, 100, &ready);
    CHECK(tb_pqueue_empty(&ready));
    tb_timerwheel_advance(&w, 104, &ready);
    pop_expect(&ready, 1);
    CHECK(w.pending == 2);

    /* До 20000: overflow-задача каскадом уходит в колесо и срабатывает. */
    tb_timerwheel_advance(&w, 20000, &ready);
    pop_expect(&ready, 4);
    CHECK(w.pending == 1);
    CHECK(w.cascaded >= 1);

    /* Глубокий overflow: до 44000 ещё ждёт; в 46000 каскад в колесо,
     * но дедлайн не наступил. */
    tb_timerwheel_advance(&w, 44000, &ready);
    CHECK(tb_pqueue_empty(&ready));
    CHECK(w.pending == 1);
    tb_timerwheel_advance(&w, 46000, &ready);
    CHECK(tb_pqueue_empty(&ready));
    CHECK(w.pending == 1);
    tb_timerwheel_advance(&w, 60000, &ready);
    pop_expect(&ready, 5);
    CHECK(w.pending == 0);
    CHECK(!tb_timerwheel_next_deadline(&w, &dl));

    /* Отмена: помеченная задача доставляется с флагом, цикл её отсеет. */
    struct tb_task *tx = mk(9, 70000);
    tb_timerwheel_add(&w, tx);
    tb_atomic_store_bool(&tx->canceled, true);
    tb_timerwheel_advance(&w, 70000, &ready);
    struct tb_task *got = tb_pqueue_pop(&ready);
    CHECK(got == tx && tb_atomic_load_bool(&got->canceled));

    tb_task_free(t100); tb_task_free(t40a); tb_task_free(t40c);
    tb_task_free(t20k); tb_task_free(t60k); tb_task_free(tx);
    tb_pqueue_destroy(&ready);
    tb_timerwheel_destroy(&w);

    printf("OK test_timer\n");
    return 0;
}
