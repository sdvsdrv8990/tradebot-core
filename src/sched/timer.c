#define TB_CURRENT_GROUP TB_G_SCHED /* зона ответственности файла */
/* ==========================================================================
 *  tb/sched/timer.c — хешированное таймерное колесо с overflow-кучей.
 *
 *  Слот = ceil(delay / SLOT_MS). Дедлайны дальше горизонта лежат в куче
 *  overflow; на каждом advance() всё достижимое каскадом переносится в
 *  слоты. При срабатывании слота задачам с ещё не наступившим дедлайном
 *  (следующий оборот колеса) разрешается остаться в слоте.
 *
 *  Контракт добавления: deadline_ms > processed_ms на момент вызова
 *  (просроченное цикл кладёт сразу в ready, минуя колесо).
 * ========================================================================== */
#include "tb/sched/timer.h"

#include <string.h>

bool tb_timerwheel_init(tb_timerwheel_t *w)
{
    if (!w) return false;
    memset(w, 0, sizeof(*w));
    return tb_pqueue_init(&w->overflow, tb_task_cmp_deadline, 64);
}

void tb_timerwheel_destroy(tb_timerwheel_t *w)
{
    if (!w) return;
    /* Задачи не освобождаем: ими владеет цикл через таблицу активных. */
    tb_pqueue_destroy(&w->overflow);
}

/* Доставка в ready. Возврат false — нехватка памяти. */
static bool deliver(tb_timerwheel_t *w, struct tb_task *t, tb_pqueue_t *ready)
{
    if (ready && tb_pqueue_push(ready, t)) {
        t->in_heap = true;
        t->state = TB_TASK_QUEUED;
        w->pending--;
        return true;
    }
    return false;
}

/* Помещение задачи: немедленная доставка / слот / overflow. */
static void wheel_place(tb_timerwheel_t *w, struct tb_task *t, tb_pqueue_t *ready)
{
    if (t->deadline_ms <= w->processed_ms) {
        if (deliver(w, t, ready)) {
            w->fired++;
        } else {
            /* Потеря памяти на ready-куче: помечаем отменённой, цикл
             * освободит при выборке; в статистику не попадает. */
            tb_atomic_store_bool(&t->canceled, true);
            w->pending--;
        }
        return;
    }
    uint64_t delay = t->deadline_ms - w->processed_ms;
    uint64_t steps = (delay + TB_TW_SLOT_MS - 1) / TB_TW_SLOT_MS; /* ceil */
    if (steps >= TB_TW_SLOTS) {
        if (tb_pqueue_push(&w->overflow, t)) {
            t->in_heap = true;
            t->in_wheel = false;
        } else {
            tb_atomic_store_bool(&t->canceled, true);
            w->pending--;
        }
        return;
    }
    uint32_t slot = (uint32_t)((w->cur + steps) % TB_TW_SLOTS);
    t->next = w->slots[slot];
    w->slots[slot] = t;
    t->in_wheel = true;
    t->in_heap = false;
}

void tb_timerwheel_add(tb_timerwheel_t *w, struct tb_task *t)
{
    if (!w || !t) return;
    t->next = NULL;
    w->pending++;
    wheel_place(w, t, NULL);
}

static void expire_slot(tb_timerwheel_t *w, uint32_t slot, uint64_t now_ms,
                        tb_pqueue_t *ready)
{
    struct tb_task *list = w->slots[slot];
    w->slots[slot] = NULL;

    while (list) {
        struct tb_task *t = list;
        list = t->next;
        t->next = NULL;
        t->in_wheel = false;

        if (t->deadline_ms <= now_ms) {
            if (deliver(w, t, ready)) {
                w->fired++;
            } else {
                tb_atomic_store_bool(&t->canceled, true);
                w->pending--;
            }
        } else {
            /* Не его оборот: вернуть в слот. */
            t->next = w->slots[slot];
            w->slots[slot] = t;
            t->in_wheel = true;
        }
    }
}

/* Каскад: переносим из overflow всё, что уместилось в горизонт колеса
 * от текущего processed_ms. Условие обязано гарантировать прогресс
 * (иначе задача вернулась бы в кучу и цикл завис). */
static void tb_timerwheel_cascade(tb_timerwheel_t *w, tb_pqueue_t *ready)
{
    for (;;) {
        struct tb_task *t = tb_pqueue_peek(&w->overflow);
        if (!t)
            break;
        if (t->deadline_ms > w->processed_ms + TB_TW_SPAN_MS - TB_TW_SLOT_MS)
            break;
        tb_pqueue_pop(&w->overflow);
        t->in_heap = false;
        w->cascaded++;
        wheel_place(w, t, ready);
    }
}

void tb_timerwheel_advance(tb_timerwheel_t *w, uint64_t now_ms, tb_pqueue_t *ready)
{
    if (!w || !ready) return;

    /* Каскад до тиков: приближение дальних задач. */
    tb_timerwheel_cascade(w, ready);

    /* 2. Тики колеса до now_ms. */
    while (w->processed_ms + TB_TW_SLOT_MS <= now_ms) {
        w->processed_ms += TB_TW_SLOT_MS;
        w->cur = (w->cur + 1) % TB_TW_SLOTS;
        expire_slot(w, w->cur, now_ms, ready);
    }

    /* 3. Каскад после тиков: processed вырос — задачи, ставшие
     * достижимыми за время большого скачка, доставляются немедленно
     * (иначе они ждали бы следующего advance). */
    tb_timerwheel_cascade(w, ready);
}

bool tb_timerwheel_next_deadline(const tb_timerwheel_t *w, uint64_t *deadline_ms)
{
    if (!w || !deadline_ms || w->pending == 0)
        return false;
    /* Ближайший тик колеса; overflow-куча может дать более точный дедлайн. */
    uint64_t best = w->processed_ms + TB_TW_SLOT_MS;
    const struct tb_task *far = tb_pqueue_peek(&w->overflow);
    if (far && far->deadline_ms < best)
        best = far->deadline_ms;
    *deadline_ms = best;
    return true;
}
