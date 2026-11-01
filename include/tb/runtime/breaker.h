/* ==========================================================================
 *  tb/runtime/breaker.h — circuit breaker на источник данных.
 *
 *  CLOSED  -> (порог подряд идущих сбоев) -> OPEN
 *  OPEN    -> (период охлаждения)         -> HALF_OPEN (ограниченные пробы)
 *  HALF_OPEN -> успех -> CLOSED; сбой -> снова OPEN.
 *
 *  Счёт сбоев ведётся только по ретраебельным сетевым/серверным ошибкам;
 *  ошибки авторизации и некорректных запросов предохранитель не открывают
 *  (повторами они не исправляются).
 * ========================================================================== */
#ifndef TB_RUNTIME_BREAKER_H
#define TB_RUNTIME_BREAKER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TB_BREAKER_CLOSED    = 0,
    TB_BREAKER_OPEN      = 1,
    TB_BREAKER_HALF_OPEN = 2
} tb_breaker_state_t;

typedef struct tb_breaker {
    uint32_t           failure_threshold;
    uint64_t           cooldown_ms;
    tb_breaker_state_t state;
    uint32_t           consecutive_failures;
    uint32_t           half_open_probes;     /* текущие пробы HALF_OPEN */
    uint32_t           half_open_max_probes; /* обычно 1 */
    uint64_t           opened_at_ms;
    /* статистика */
    uint64_t           transitions;
    uint64_t           rejected_total;
} tb_breaker_t;

void tb_breaker_init(tb_breaker_t *b, uint32_t failure_threshold, uint64_t cooldown_ms);

/* Разрешён ли вызов источника в момент now_ms. В HALF_OPEN резервирует пробу. */
bool tb_breaker_allow(tb_breaker_t *b, uint64_t now_ms);

/* Успех: сбрасывает серию сбоев, HALF_OPEN -> CLOSED. */
void tb_breaker_success(tb_breaker_t *b);

/* Сбой: наращивает серию, при пороге — OPEN; в HALF_OPEN — сразу OPEN. */
void tb_breaker_failure(tb_breaker_t *b, uint64_t now_ms);

/* Остаток времени до перехода OPEN -> HALF_OPEN (для планирования). */
uint64_t tb_breaker_retry_in_ms(const tb_breaker_t *b, uint64_t now_ms);

/* Момент, когда проба HALF_OPEN станет доступна: opened_at_ms + cooldown_ms.
 * Именно это значение кладётся в контекст события при блокировке шагом
 * breaker — арифметика никогда не подразумевается на стороне вызывающего
 * (урок теста: открыт в t=2 при охлаждении 100 мс -> проба с t=102, не 100).
 * В состояниях, отличных от OPEN, возвращает 0. */
uint64_t tb_breaker_probe_at_ms(const tb_breaker_t *b);

const char *tb_breaker_state_name(const tb_breaker_t *b);

#ifdef __cplusplus
}
#endif
#endif /* TB_RUNTIME_BREAKER_H */
