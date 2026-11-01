/* ==========================================================================
 *  tb/events/builtins.h — готовые шаги цепочки и наблюдатели.
 *
 *  Это «конструктор» типового конвейера Universal API Runtime поверх
 *  реестра событий: шлюзы политики повтора как шаги цепочки и подписка
 *  Event Ledger на терминальные состояния. Пользовательские шаги (модули
 *  1–4, Decision Engine) пишутся так же — функция + контекст.
 *
 *  Контекст шагов-шлюзов сознательно многословен: урок теста breaker
 *  («открыт в t=2, охлаждение 100 -> проба с t=102, а не 100») — в след
 *  события попадают сами величины (opened_at, cooldown, probe_at),
 *  арифметику не приходится подразумевать.
 * ========================================================================== */
#ifndef TB_EVENTS_BUILTINS_H
#define TB_EVENTS_BUILTINS_H

#include "tb/events/events.h"
#include "tb/ledger/ledger.h"
#include "tb/runtime/breaker.h"
#include "tb/runtime/ratelimit.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Шаг «rate limit» (token-bucket на источник) ----
 * user — tb_evmod_ratelimit_t*. Токен есть — CHAIN; нет — полный повтор
 * (tb_event_retry_at) до момента появления токена: на wake шлюз
 * перепроверяется, токен мог достаться другому событию. */
typedef struct tb_evmod_ratelimit {
    tb_ratelimit_t *rl;
    uint64_t      (*now_ms)(void);   /* NULL -> tb_now_ms(); инъекция для тестов */
} tb_evmod_ratelimit_t;

tb_ev_action_t tb_evmod_ratelimit_fn(tb_evreg_t *r, tb_event_t *ev, void *user);

/* ---- Шаг «circuit breaker» (предохранитель источника) ----
 * user — tb_evmod_breaker_t*. Разрешено — CHAIN (в том числе проба
 * HALF_OPEN); разомкнуто — полный повтор (tb_event_retry_at) до
 * probe_at = opened_at + cooldown, причём обе величины и итог явно
 * попадают в след события. */
typedef struct tb_evmod_breaker {
    tb_breaker_t *br;
    uint64_t    (*now_ms)(void);     /* NULL -> tb_now_ms(); инъекция для тестов */
} tb_evmod_breaker_t;

tb_ev_action_t tb_evmod_breaker_fn(tb_evreg_t *r, tb_event_t *ev, void *user);

/* ---- Наблюдатель «Event Ledger» ----
 * Подписывается на терминальные состояния и пишет запись события в
 * единый журнал (JSONL, type="event.trace"): id реестра, kind, состояние,
 * попытки/шаги, ошибка (POD-значением) и весь текстовый след шагов.
 * После записи событие закрывается — live-таблица держит только живые.
 * Возврат — индекс подписки или -1 (журнал не открыт). */
int tb_ev_ledger_attach(tb_evreg_t *r, tb_ledger_t *ledger);

#ifdef __cplusplus
}
#endif
#endif /* TB_EVENTS_BUILTINS_H */
