/* ==========================================================================
 *  tb/sched/timer.h — хешированное таймерное колесо.
 *
 *  Разрешение слота 8 мс, 2048 слоёв -> горизонт ~16.4 с. Дедлайны дальше
 *  горизонта живут в overflow-куче и каскадируются в колесо по мере
 *  приближения. Срабатывание — доставка задачи в ready-очередь цикла.
 *
 *  Колесо не владеет задачами и не знает про освобождение: отмена — это
 *  атомарная пометка canceled, физическое освобождение выполняет цикл.
 *
 *  Функции колеса — чистые по отношению к переданному времени now_ms,
 *  что даёт детерминированное тестирование без реального спячего ожидания.
 * ========================================================================== */
#ifndef TB_SCHED_TIMER_H
#define TB_SCHED_TIMER_H

#include <stdbool.h>
#include <stdint.h>
#include "tb/sched/pqueue.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TB_TW_SLOT_MS  8ull
#define TB_TW_SLOTS    2048u
#define TB_TW_SPAN_MS  (TB_TW_SLOT_MS * (uint64_t)TB_TW_SLOTS) /* ~16.4 с */

typedef struct tb_timerwheel {
    uint64_t          processed_ms; /* виртуальное «сейчас», кратно SLOT_MS */
    uint32_t          cur;          /* текущий слот */
    struct tb_task   *slots[TB_TW_SLOTS];
    tb_pqueue_t       overflow;     /* далёкие дедлайны (куча по deadline) */
    uint64_t          pending;      /* задач в колесе и overflow */
    uint64_t          fired;
    uint64_t          cascaded;
} tb_timerwheel_t;

bool tb_timerwheel_init(tb_timerwheel_t *w);
void tb_timerwheel_destroy(tb_timerwheel_t *w);

/* t->deadline_ms (абсолютное монотонное) должен быть установлен. */
void tb_timerwheel_add(tb_timerwheel_t *w, struct tb_task *t);

/* Продвижение до now_ms: задачи с deadline <= now_ms уходят в ready
 * (cmp_ready). Отменённые доставляются тоже — их отсеет цикл. */
void tb_timerwheel_advance(tb_timerwheel_t *w, uint64_t now_ms, tb_pqueue_t *ready);

/* Ближайший дедлайн (колесо или overflow); false — таймеров нет. */
bool tb_timerwheel_next_deadline(const tb_timerwheel_t *w, uint64_t *deadline_ms);

#ifdef __cplusplus
}
#endif
#endif /* TB_SCHED_TIMER_H */
