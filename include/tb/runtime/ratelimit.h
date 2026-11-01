/* ==========================================================================
 *  tb/runtime/ratelimit.h — token-bucket на источник данных.
 *
 *  Все обращения к источнику проходят через общий ограничитель
 *  (API Scheduler / Rate Limiter), поэтому конкурирующие модули не
 *  устраивают гонку за общий лимит. Лимит задаётся конфигурацией
 *  поставщика и может быть консервативнее текущего публичного
 *  (проектируемся под 10 запросов/мин, даже если источник допускает 30).
 * ========================================================================== */
#ifndef TB_RUNTIME_RATELIMIT_H
#define TB_RUNTIME_RATELIMIT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tb_ratelimit {
    uint32_t rate_per_min;   /* скорость пополнения */
    uint32_t burst;          /* ёмкость корзины */
    double   tokens;         /* текущее число токенов */
    uint64_t last_ms;        /* момент последнего пополнения */
    /* статистика */
    uint64_t granted;
    uint64_t deferred;
} tb_ratelimit_t;

void tb_ratelimit_init(tb_ratelimit_t *rl, uint32_t rate_per_min, uint32_t burst);

/* Попытаться взять токен в момент now_ms. false — лимит, см. next_slot. */
bool tb_ratelimit_acquire(tb_ratelimit_t *rl, uint64_t now_ms);

/* Через сколько мс появится следующий токен (0 — есть прямо сейчас). */
uint64_t tb_ratelimit_next_slot_ms(const tb_ratelimit_t *rl, uint64_t now_ms);

#ifdef __cplusplus
}
#endif
#endif /* TB_RUNTIME_RATELIMIT_H */
