/* ==========================================================================
 *  tb/json/envelope.h — API Response Envelope: единый формат ответов.
 *
 *  Независимо от источника (CoinGecko, GeckoTerminal, GoPlus, DefiLlama)
 *  ответ нормализуется в одну структуру с тремя статусами:
 *    SUCCESS — данные получены;
 *    ERROR   — ошибка (унифицированный код из реестра, Retry-After и т.д.);
 *    PENDING — незавершённая задача (202 Accepted / фонная обработка).
 *
 *  Схема 1.0 (поле error — только при ERROR; retry — при ERROR/PENDING):
 *    {"schema_version":"1.0","source":"geckoterminal",
 *     "operation":"get_pool_ohlcv","status":"SUCCESS",
 *     "request_id":142,"timestamp":1793470000123,
 *     "data":{...}|null,
 *     "error":{"code":"TB_API_RATE_LIMIT","domain":"API","http_status":429,
 *              "severity":"WARN","retryable":true,"retry_after_ms":3000,
 *              "message":"..."}|null,
 *     "retry":{"attempt":2,"next_attempt_at":1793470040123}|null}
 *
 *  Реализация — C++/rapidjson с C-ABI, чтобы C-ядро пользовалось без
 *  зависимостей от C++ типов. Все строки копируются в фиксированные буферы.
 * ========================================================================== */
#ifndef TB_JSON_ENVELOPE_H
#define TB_JSON_ENVELOPE_H

#include <stddef.h>
#include <stdint.h>
#include "tb/core/error.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TB_ENV_SCHEMA_VERSION "1.0"
#define TB_ENV_SRC_MAX 32
#define TB_ENV_OP_MAX 48
#define TB_ENV_DATA_MAX 1024

typedef enum {
    TB_ENV_SUCCESS = 0,
    TB_ENV_ERROR   = 1,
    TB_ENV_PENDING = 2
} tb_env_status_t;

const char *tb_env_status_name(int status);

typedef struct tb_envelope {
    char        source[TB_ENV_SRC_MAX];   /* "geckoterminal" */
    char        operation[TB_ENV_OP_MAX]; /* "get_pool_ohlcv" */
    int         status;                   /* tb_env_status_t */
    uint64_t    request_id;
    int64_t     timestamp;                /* epoch ms; 0 — взять текущее */
    char        data_json[TB_ENV_DATA_MAX]; /* JSON-объект или пусто (=null) */
    tb_error_t  error;                    /* валиден при status == ERROR */
    uint32_t    attempt;                  /* номер попытки */
    int64_t     next_attempt_at;          /* epoch ms; -1 — нет повтора */
} tb_envelope_t;

void tb_envelope_init(tb_envelope_t *env, const char *source,
                      const char *operation, uint64_t request_id);

/* Данные: валидный JSON-объект (проверяется разбором) или NULL. */
int tb_envelope_set_data(tb_envelope_t *env, const char *data_json, tb_error_t *err);

/* Ошибка: копируется, метаданные домена/кода — из реестра. */
int tb_envelope_set_error(tb_envelope_t *env, const tb_error_t *error);

void tb_envelope_set_retry(tb_envelope_t *env, uint32_t attempt,
                           int64_t next_attempt_at /* -1 = нет */);

/* Сериализация в компактный JSON. out_len может быть NULL. */
int tb_envelope_serialize(const tb_envelope_t *env, char *buf, size_t cap,
                          size_t *out_len, tb_error_t *err);

/* Разбор строки в env. Неизвестные поля игнорируются (прямота эволюции). */
int tb_envelope_parse(const char *json, size_t len, tb_envelope_t *out,
                      tb_error_t *err);

#ifdef __cplusplus
}
#endif
#endif /* TB_JSON_ENVELOPE_H */
