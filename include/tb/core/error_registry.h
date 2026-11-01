/* ==========================================================================
 *  tb/core/error_registry.h — реестр ошибок: «база данных обработчика».
 *
 *  Здесь живёт каноническая информация о кодах:
 *    1) метаданные унифицированных кодов (имя, severity, retryable);
 *    2) шаблоны соответствия «ответ источника -> унифицированная ошибка»
 *       (HTTP-статусы и паттерны тел ответов CoinGecko, GeckoTerminal,
 *       GoPlus, DefiLlama и прочих).
 *
 *  При подключении нового API достаточно добавить правило отображения —
 *  вся остальная обработка (backoff, Retry-After, circuit breaker,
 *  Envelope) уже работает поверх унифицированных кодов.
 * ========================================================================== */
#ifndef TB_CORE_ERROR_REGISTRY_H
#define TB_CORE_ERROR_REGISTRY_H

#include "tb/core/error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Метаданные унифицированного кода ошибки. */
typedef struct {
    uint8_t      domain;            /* tb_domain_t */
    uint32_t     code;              /* код внутри домена */
    const char  *name;              /* каноническое имя, напр. "TB_API_RATE_LIMIT" */
    uint8_t      default_severity;  /* tb_severity_t */
    bool         retryable;
    const char  *description;
} tb_error_meta_t;

/* Правило отображения ответа источника в унифицированную ошибку.
 * source == "*" — правило по умолчанию для любого источника. */
typedef struct {
    const char *source;         /* "geckoterminal" | "coingecko" | ... | "*" */
    int32_t     http_status;    /* точный статус; 0 — любой статус */
    const char *pattern;        /* NULL или подстрока в теле ответа (без учёта регистра) */
    uint8_t     domain;         /* обычно TB_DOMAIN_API */
    uint32_t    code;           /* унифицированный код */
    bool        retryable;      /* переопределяет retryable из метаданных */
    const char *note;
} tb_provider_map_t;

/* ---- Метаданные кодов ---- */
const tb_error_meta_t *tb_registry_meta(uint8_t domain, uint32_t code);
size_t tb_registry_meta_count(void);
const tb_error_meta_t *tb_registry_meta_at(size_t index);

/* Обратный поиск по каноническому имени ("TB_API_RATE_LIMIT"). */
bool tb_registry_code_by_name(const char *name, uint8_t *domain, uint32_t *code);

/* ---- Отображение ответов источников ----
 * Порядок разрешения правил:
 *   1. source + http_status + pattern;
 *   2. source + http_status;
 *   3. "*" + http_status;
 *   4. эвристика по классу статуса (429 -> RATE_LIMIT, 5xx -> SERVER,
 *      401/403 -> UNAUTHORIZED, 202 -> PENDING и т.д.).
 * retry_after_ms >= 0 попадает в ошибку как есть (уже в миллисекундах).
 * Возвращает true, если ответ считается ошибкой и err заполнен;
 * false — статус успешный (2xx), err очищен. */
bool tb_registry_map_response(const char *source, int32_t http_status,
                              const char *body, size_t body_len,
                              int64_t retry_after_ms,
                              tb_error_t *err);

/* ---- Расширение реестра собственными правилами (новые источники) ----
 * Потокобезопасно (внутренний spinlock). Лимит пользовательских правил —
 * TB_REGISTRY_MAX_CUSTOM; при переполнении — false с ошибкой в err. */
#define TB_REGISTRY_MAX_CUSTOM 64
bool tb_registry_add_source_map(const tb_provider_map_t *rule, tb_error_t *err);

size_t tb_registry_map_count(void);
const tb_provider_map_t *tb_registry_map_at(size_t index);

/* Заполнить severity/retryable ошибки метаданными реестра (если код известен). */
void tb_registry_decorate(tb_error_t *err);

#ifdef __cplusplus
}
#endif
#endif /* TB_CORE_ERROR_REGISTRY_H */
