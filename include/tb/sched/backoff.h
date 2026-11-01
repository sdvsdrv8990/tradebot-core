/* ==========================================================================
 *  tb/sched/backoff.h — адаптивная задержка повтора: экспонента + jitter.
 *
 *  Правила (зафиксированы в обсуждении архитектуры):
 *   - интервал растёт экспоненциально от базы, с потолком;
 *   - к интервалу примешивается случайное отклонение (jitter), чтобы
 *     одновременно работающие модули не синхронизировались в всплеск;
 *   - если источник вернул Retry-After, итоговая задержка не может быть
 *     меньше разрешённого времени (jitter не даёт спросить раньше срока);
 *   - после успеха счётчик попыток обнуляет вызывающий (attempt = 0);
 *   - неретраебельная ошибка не наращивает ожидание вообще — возврат 0.
 * ========================================================================== */
#ifndef TB_SCHED_BACKOFF_H
#define TB_SCHED_BACKOFF_H

#include <stdbool.h>
#include <stdint.h>
#include "tb/core/error.h"
#include "tb/sched/task.h" /* tb_retry_policy_t */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tb_backoff {
    uint64_t base_ms;      /* база экспоненты */
    uint64_t cap_ms;       /* потолок задержки */
    double   jitter_frac;  /* доля случайного отклонения, 0..1 */
    uint64_t rng_state;    /* xorshift64* — состояние ГПСЧ */
} tb_backoff_t;

void tb_backoff_init(tb_backoff_t *b, uint64_t base_ms, uint64_t cap_ms,
                     double jitter_frac, uint64_t seed);

/* Задержка перед попыткой №attempt (1 — первый повтор).
 * Для неретраебельной ошибки возвращает 0 (повтор не планируется).
 * Итог всегда >= err->retry_after_ms, если тот задан. */
uint64_t tb_backoff_delay(tb_backoff_t *b, uint32_t attempt, const tb_error_t *err);

/* Проверка: исчерпаны ли попытки согласно политике. */
bool tb_retry_exhausted(const tb_retry_policy_t *policy, uint32_t attempt);

/* ГПСЧ (xorshift64*). Максимум готового: используется и для jitter,
 * и для симуляций. */
uint64_t tb_rand_next(tb_backoff_t *b);

#ifdef __cplusplus
}
#endif
#endif /* TB_SCHED_BACKOFF_H */
