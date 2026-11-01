/* ==========================================================================
 *  tb/core/error_registry.c — «база данных обработчика»:
 *  канонические метаданные кодов + шаблоны отображения ответов источников.
 *
 *  Правила провайдеров ниже — стартовый набор, иллюстративный по телам
 *  ответов (точные форматы уточняются адаптерами при подключении реальных
 *  HTTP-вызовов). Расширение — через tb_registry_add_source_map() или
 *  добавлением строк в tb_builtin_maps.
 * ========================================================================== */
#include "tb/core/error_registry.h"
#include "tb/arch/arch.h"
#include "tb/exec/codes.h" /* коды домена USER (Execution Engine) */

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* ---- Метаданные унифицированных кодов ---- */
static const tb_error_meta_t tb_builtin_meta[] = {
    /* CORE */
    { TB_DOMAIN_CORE, TB_CORE_OK,            "TB_CORE_OK",            TB_SEV_INFO,  false, "успех" },
    { TB_DOMAIN_CORE, TB_CORE_NO_MEMORY,     "TB_CORE_NO_MEMORY",     TB_SEV_FATAL, false, "нехватка памяти" },
    { TB_DOMAIN_CORE, TB_CORE_INVALID_ARG,   "TB_CORE_INVALID_ARG",   TB_SEV_ERROR, false, "недопустимый аргумент" },
    { TB_DOMAIN_CORE, TB_CORE_NOT_INIT,      "TB_CORE_NOT_INIT",      TB_SEV_ERROR, false, "компонент не инициализирован" },
    { TB_DOMAIN_CORE, TB_CORE_UNSUPPORTED,   "TB_CORE_UNSUPPORTED",   TB_SEV_ERROR, false, "операция не поддерживается" },
    { TB_DOMAIN_CORE, TB_CORE_OVERFLOW,      "TB_CORE_OVERFLOW",      TB_SEV_ERROR, false, "переполнение счётчика/буфера" },
    { TB_DOMAIN_CORE, TB_CORE_IO,            "TB_CORE_IO",            TB_SEV_ERROR, false, "ошибка ввода-вывода" },
    /* SCHED */
    { TB_DOMAIN_SCHED, TB_SCHED_OK,             "TB_SCHED_OK",             TB_SEV_INFO,  false, "успех" },
    { TB_DOMAIN_SCHED, TB_SCHED_TASK_NOT_FOUND, "TB_SCHED_TASK_NOT_FOUND", TB_SEV_WARN,  false, "задача не найдена (завершена ранее)" },
    { TB_DOMAIN_SCHED, TB_SCHED_SHUTDOWN,       "TB_SCHED_SHUTDOWN",       TB_SEV_WARN,  false, "цикл останавливается" },
    { TB_DOMAIN_SCHED, TB_SCHED_POOL_CLOSED,    "TB_SCHED_POOL_CLOSED",    TB_SEV_WARN,  false, "пул потоков закрыт" },
    { TB_DOMAIN_SCHED, TB_SCHED_TOO_MANY_WATCH, "TB_SCHED_TOO_MANY_WATCH", TB_SEV_ERROR, false, "исчерпан лимит наблюдений fd" },
    /* NET — транспорт HTTP/TLS */
    { TB_DOMAIN_NET, TB_NET_OK,       "TB_NET_OK",       TB_SEV_INFO,  false, "успех" },
    { TB_DOMAIN_NET, TB_NET_DNS,      "TB_NET_DNS",      TB_SEV_WARN,  true,  "резолв хоста не удался" },
    { TB_DOMAIN_NET, TB_NET_CONNECT,  "TB_NET_CONNECT",  TB_SEV_WARN,  true,  "соединение не установлено" },
    { TB_DOMAIN_NET, TB_NET_TLS,      "TB_NET_TLS",      TB_SEV_ERROR, true,  "TLS-рукопожатие или сертификат отклонён" },
    { TB_DOMAIN_NET, TB_NET_TIMEOUT,  "TB_NET_TIMEOUT",  TB_SEV_WARN,  true,  "таймаут чтения/записи" },
    { TB_DOMAIN_NET, TB_NET_BAD_URL,  "TB_NET_BAD_URL",  TB_SEV_ERROR, false, "url не разобран" },
    { TB_DOMAIN_NET, TB_NET_RESPONSE, "TB_NET_RESPONSE", TB_SEV_ERROR, false, "ответ не соответствует HTTP" },
    { TB_DOMAIN_NET, TB_NET_CLOSED,   "TB_NET_CLOSED",   TB_SEV_WARN,  true,  "соединение закрыто до полного ответа" },
    /* LEDGER */
    { TB_DOMAIN_LEDGER, TB_LEDGER_OK,       "TB_LEDGER_OK",       TB_SEV_INFO,  false, "успех" },
    { TB_DOMAIN_LEDGER, TB_LEDGER_IO,       "TB_LEDGER_IO",       TB_SEV_ERROR, false, "ошибка записи журнала" },
    { TB_DOMAIN_LEDGER, TB_LEDGER_BAD_JSON, "TB_LEDGER_BAD_JSON", TB_SEV_ERROR, false, "некорректный JSON в payload события" },
    { TB_DOMAIN_LEDGER, TB_LEDGER_DUP,      "TB_LEDGER_DUP",      TB_SEV_INFO,  false, "дубликат события пропущен (дедупликация по uuid)" },
    /* JSON */
    { TB_DOMAIN_JSON, TB_JSON_OK,      "TB_JSON_OK",      TB_SEV_INFO,  false, "успех" },
    { TB_DOMAIN_JSON, TB_JSON_PARSE,   "TB_JSON_PARSE",   TB_SEV_ERROR, false, "синтаксическая ошибка JSON" },
    { TB_DOMAIN_JSON, TB_JSON_SCHEMA,  "TB_JSON_SCHEMA",  TB_SEV_ERROR, false, "JSON не соответствует схеме Envelope" },
    { TB_DOMAIN_JSON, TB_JSON_BUFFER,  "TB_JSON_BUFFER",  TB_SEV_ERROR, false, "буфер результата слишком мал" },
    /* EVENTS — реестр событий (цепочка модулей, live-таблица, прогон) */
    { TB_DOMAIN_EVENTS, TB_EV_OK,         "TB_EV_OK",         TB_SEV_INFO,  false, "успех" },
    { TB_DOMAIN_EVENTS, TB_EV_BAD_HANDLE, "TB_EV_BAD_HANDLE", TB_SEV_ERROR, false, "неверный реестр/событие/имя шага" },
    { TB_DOMAIN_EVENTS, TB_EV_NO_SLOT,    "TB_EV_NO_SLOT",    TB_SEV_ERROR, false, "таблица модулей или live-таблица переполнена" },
    { TB_DOMAIN_EVENTS, TB_EV_NO_MODULE,  "TB_EV_NO_MODULE",  TB_SEV_ERROR, false, "шаг цепочки не найден" },
    { TB_DOMAIN_EVENTS, TB_EV_DUPLICATE,  "TB_EV_DUPLICATE",  TB_SEV_ERROR, false, "имя шага уже занято" },
    { TB_DOMAIN_EVENTS, TB_EV_BAD_STATE,  "TB_EV_BAD_STATE",  TB_SEV_ERROR, false, "операция несовместима с состоянием события" },
    { TB_DOMAIN_EVENTS, TB_EV_RECURSION,  "TB_EV_RECURSION",  TB_SEV_ERROR, false, "вложенный прогон цепочки запрещён" },
    { TB_DOMAIN_EVENTS, TB_EV_CONTRACT,   "TB_EV_CONTRACT",   TB_SEV_ERROR, false, "шаг нарушил контракт прогона" },
    /* API — стандартизация ответов источников данных */
    { TB_DOMAIN_API, TB_API_OK,           "TB_API_OK",           TB_SEV_INFO,  false, "успешный ответ источника" },
    { TB_DOMAIN_API, TB_API_RATE_LIMIT,   "TB_API_RATE_LIMIT",   TB_SEV_WARN,  true,  "HTTP 429: лимит запросов; повтор с учётом Retry-After" },
    { TB_DOMAIN_API, TB_API_TIMEOUT,      "TB_API_TIMEOUT",      TB_SEV_WARN,  true,  "источник не ответил вовремя" },
    { TB_DOMAIN_API, TB_API_NETWORK,      "TB_API_NETWORK",      TB_SEV_WARN,  true,  "сетевая ошибка (обрыв, DNS)" },
    { TB_DOMAIN_API, TB_API_SERVER,       "TB_API_SERVER",       TB_SEV_WARN,  true,  "5xx на стороне источника" },
    { TB_DOMAIN_API, TB_API_UNAUTHORIZED, "TB_API_UNAUTHORIZED", TB_SEV_ERROR, false, "401/403: ключ отклонён, повторами не исправляется" },
    { TB_DOMAIN_API, TB_API_NOT_FOUND,    "TB_API_NOT_FOUND",    TB_SEV_WARN,  false, "404: объект неизвестен источнику" },
    { TB_DOMAIN_API, TB_API_BAD_REQUEST,  "TB_API_BAD_REQUEST",  TB_SEV_ERROR, false, "400/422: некорректный запрос (например, адрес токена)" },
    { TB_DOMAIN_API, TB_API_PARSE,        "TB_API_PARSE",        TB_SEV_ERROR, false, "ответ не соответствует ожидаемому формату" },
    { TB_DOMAIN_API, TB_API_PENDING,      "TB_API_PENDING",      TB_SEV_INFO,  true,  "202 Accepted: фоновая задача источника, продолжить опрос" },
    { TB_DOMAIN_API, TB_API_CIRCUIT_OPEN, "TB_API_CIRCUIT_OPEN", TB_SEV_WARN,  true,  "наш circuit breaker разомкнут для источника" },
    { TB_DOMAIN_API, TB_API_CANCELED,     "TB_API_CANCELED",     TB_SEV_INFO,  false, "операция отменена" },
    { TB_DOMAIN_API, TB_API_UNSUPPORTED,  "TB_API_UNSUPPORTED",  TB_SEV_ERROR, false, "источник не поддерживает операцию" },
    /* USER / Execution Engine — механизм торговли (домен описан в tb/exec/codes.h) */
    { TB_DOMAIN_USER, TB_EXEC_OK,                     "TB_EXEC_OK",                     TB_SEV_INFO,  false, "успех" },
    { TB_DOMAIN_USER, TB_EXEC_ZERO_AMOUNT,            "TB_EXEC_ZERO_AMOUNT",            TB_SEV_ERROR, false, "нулевая сумма входа (ZeroAmount)" },
    { TB_DOMAIN_USER, TB_EXEC_DEADLINE_PAST,          "TB_EXEC_DEADLINE_PAST",          TB_SEV_ERROR, false, "дедлайн ордера истёк или слишком близко (DeadlineInPast)" },
    { TB_DOMAIN_USER, TB_EXEC_INVALID_PATH,           "TB_EXEC_INVALID_PATH",           TB_SEV_ERROR, false, "маршрут не является путём V3 (InvalidPath)" },
    { TB_DOMAIN_USER, TB_EXEC_VALUE_MISMATCH,         "TB_EXEC_VALUE_MISMATCH",         TB_SEV_ERROR, false, "дебет/кредит слота не сходится (WrongMsgValue)" },
    { TB_DOMAIN_USER, TB_EXEC_SETTLE_FAILED,          "TB_EXEC_SETTLE_FAILED",          TB_SEV_ERROR, false, "фиксация расчёта не удалась (EthTransferFailed)" },
    { TB_DOMAIN_USER, TB_EXEC_NOT_WRAPPED,            "TB_EXEC_NOT_WRAPPED",            TB_SEV_ERROR, false, "нативный актив минуя unwrap — инвариант receive() (NotWeth)" },
    { TB_DOMAIN_USER, TB_EXEC_SLIPPAGE,               "TB_EXEC_SLIPPAGE",               TB_SEV_WARN,  false, "выход меньше amountOutMinimum (slippage)" },
    { TB_DOMAIN_USER, TB_EXEC_ROUTE_MISMATCH,         "TB_EXEC_ROUTE_MISMATCH",         TB_SEV_ERROR, false, "маршрут не согласован с ордером" },
    { TB_DOMAIN_USER, TB_EXEC_INSUFFICIENT_BALANCE,   "TB_EXEC_INSUFFICIENT_BALANCE",   TB_SEV_ERROR, false, "не хватает виртуального баланса на вход" },
    { TB_DOMAIN_USER, TB_EXEC_INSUFFICIENT_LIQUIDITY, "TB_EXEC_INSUFFICIENT_LIQUIDITY", TB_SEV_WARN,  false, "бэкенд: ликвидности маршрута мало" },
    { TB_DOMAIN_USER, TB_EXEC_ALLOWLIST,              "TB_EXEC_ALLOWLIST",              TB_SEV_ERROR, false, "токен вне разрешённого списка" },
    { TB_DOMAIN_USER, TB_EXEC_NOTIONAL_LIMIT,         "TB_EXEC_NOTIONAL_LIMIT",         TB_SEV_ERROR, false, "сумма ордера выше лимита" },
    { TB_DOMAIN_USER, TB_EXEC_HOPS_LIMIT,             "TB_EXEC_HOPS_LIMIT",             TB_SEV_ERROR, false, "слишком много хопов в маршруте" },
    { TB_DOMAIN_USER, TB_EXEC_OVERFLOW,               "TB_EXEC_OVERFLOW",               TB_SEV_ERROR, false, "переполнение 256-битной суммы" },
    { TB_DOMAIN_USER, TB_EXEC_TIMEOUT,                "TB_EXEC_TIMEOUT",                TB_SEV_WARN,  true,  "PENDING не заполнился за лимит опросов" },
    { TB_DOMAIN_USER, TB_EXEC_BACKEND,                "TB_EXEC_BACKEND",                TB_SEV_WARN,  true,  "ошибка бэкенда исполнения (DEX)" },
    { TB_DOMAIN_USER, TB_EXEC_CAPACITY,               "TB_EXEC_CAPACITY",               TB_SEV_ERROR, false, "таблица ордеров/кошелька переполнена" },
    { TB_DOMAIN_USER, TB_EXEC_BAD_DECIMALS,           "TB_EXEC_BAD_DECIMALS",           TB_SEV_ERROR, false, "неверная десятичная строка суммы" },
};

/* ---- Шаблоны отображения ответов источников ---- */
static const tb_provider_map_t tb_builtin_maps[] = {
    /* GeckoTerminal (пулы, OHLCV; публичный API в beta) */
    { "geckoterminal", 429, NULL,          TB_DOMAIN_API, TB_API_RATE_LIMIT,  true,  "лимит запросов, учитывать Retry-After" },
    { "geckoterminal", 400, "invalid pool",TB_DOMAIN_API, TB_API_BAD_REQUEST, false, "неизвестный идентификатор пула" },
    { "geckoterminal", 404, NULL,          TB_DOMAIN_API, TB_API_NOT_FOUND,   false, "пул/сеть не найдены" },
    /* CoinGecko (рыночные данные, история) */
    { "coingecko", 429, NULL,           TB_DOMAIN_API, TB_API_RATE_LIMIT,  true,  "лимит запросов, учитывать Retry-After" },
    { "coingecko", 429, "rate limit",   TB_DOMAIN_API, TB_API_RATE_LIMIT,  true,  "тело ответа подтверждает rate limit" },
    { "coingecko", 400, "invalid coin", TB_DOMAIN_API, TB_API_BAD_REQUEST, false, "неверный идентификатор монеты" },
    /* GoPlus (проверки безопасности токенов) */
    { "goplus", 429, NULL,   TB_DOMAIN_API, TB_API_RATE_LIMIT,  true,  "лимит запросов" },
    { "goplus", 403, NULL,   TB_DOMAIN_API, TB_API_UNAUTHORIZED, false, "ключ отклонён" },
    /* DefiLlama (TVL сетей и протоколов) */
    { "defillama", 429, NULL, TB_DOMAIN_API, TB_API_RATE_LIMIT, true,  "лимит запросов" },
    { "defillama", 503, NULL, TB_DOMAIN_API, TB_API_SERVER,     true,  "обслуживание на стороне источника" },
    /* Общие правила для любых источников */
    { "*", 408, NULL, TB_DOMAIN_API, TB_API_TIMEOUT,      true,  "таймаут источника" },
    { "*", 425, NULL, TB_DOMAIN_API, TB_API_RATE_LIMIT,   true,  "too early" },
    { "*", 401, NULL, TB_DOMAIN_API, TB_API_UNAUTHORIZED, false, "нет авторизации" },
    { "*", 403, NULL, TB_DOMAIN_API, TB_API_UNAUTHORIZED, false, "доступ запрещён" },
    { "*", 404, NULL, TB_DOMAIN_API, TB_API_NOT_FOUND,    false, "не найдено" },
    { "*", 400, NULL, TB_DOMAIN_API, TB_API_BAD_REQUEST,  false, "некорректный запрос" },
    { "*", 422, NULL, TB_DOMAIN_API, TB_API_BAD_REQUEST,  false, "семантическая ошибка запроса" },
    { "*", 500, NULL, TB_DOMAIN_API, TB_API_SERVER,       true,  "внутренняя ошибка источника" },
    { "*", 502, NULL, TB_DOMAIN_API, TB_API_SERVER,       true,  "шлюз недоступен" },
    { "*", 503, NULL, TB_DOMAIN_API, TB_API_SERVER,       true,  "сервис недоступен" },
    { "*", 504, NULL, TB_DOMAIN_API, TB_API_TIMEOUT,      true,  "шлюз не дождался ответа" },
    { "*", 202, NULL, TB_DOMAIN_API, TB_API_PENDING,      true,  "фоновая задача поставщика" },
};

/* ---- Пользовательские правила (расширение в рантайме) ---- */
static tb_provider_map_t g_custom_maps[TB_REGISTRY_MAX_CUSTOM];
static size_t            g_custom_count;
static tb_spinlock_t     g_registry_lock = TB_SPINLOCK_INIT;

/* ---- Метаданные ---- */
const tb_error_meta_t *tb_registry_meta(uint8_t domain, uint32_t code)
{
    for (size_t i = 0; i < sizeof(tb_builtin_meta) / sizeof(tb_builtin_meta[0]); i++)
        if (tb_builtin_meta[i].domain == domain && tb_builtin_meta[i].code == code)
            return &tb_builtin_meta[i];
    return NULL;
}

size_t tb_registry_meta_count(void)
{
    return sizeof(tb_builtin_meta) / sizeof(tb_builtin_meta[0]);
}

const tb_error_meta_t *tb_registry_meta_at(size_t index)
{
    if (index >= tb_registry_meta_count()) return NULL;
    return &tb_builtin_meta[index];
}

bool tb_registry_code_by_name(const char *name, uint8_t *domain, uint32_t *code)
{
    if (!name) return false;
    for (size_t i = 0; i < tb_registry_meta_count(); i++) {
        const tb_error_meta_t *m = &tb_builtin_meta[i];
        if (strcmp(m->name, name) == 0) {
            if (domain) *domain = m->domain;
            if (code)   *code   = m->code;
            return true;
        }
    }
    return false;
}

void tb_registry_decorate(tb_error_t *err)
{
    if (!err) return;
    const tb_error_meta_t *meta = tb_registry_meta(err->domain, err->code);
    if (meta) {
        if (err->severity == 0 && meta->default_severity != 0)
            err->severity = meta->default_severity;
        /* retryable из реестра — значение по умолчанию; правило источника
         * может переопределить его в tb_registry_map_response(). */
        err->retryable = meta->retryable;
    }
}

/* ---- Сопоставление шаблонов ---- */
static bool body_contains_ci(const char *body, size_t body_len, const char *pattern)
{
    if (!pattern || !body || body_len == 0) return true; /* пустой паттерн матчится всегда */
    size_t plen = strlen(pattern);
    if (plen == 0 || plen > body_len) return false;
    for (size_t i = 0; i + plen <= body_len; i++) {
        size_t j = 0;
        while (j < plen && tolower((unsigned char)body[i + j]) == tolower((unsigned char)pattern[j]))
            j++;
        if (j == plen) return true;
    }
    return false;
}

static bool rule_matches(const tb_provider_map_t *r, const char *source,
                         int32_t http_status, const char *body, size_t body_len)
{
    if (r->http_status != 0 && r->http_status != http_status) return false;
    if (strcmp(r->source, "*") != 0 && strcmp(r->source, source) != 0) return false;
    if (r->http_status == 0 && r->pattern == NULL) return false; /* «любой статус без паттерна» — не правило */
    return body_contains_ci(body, body_len, r->pattern);
}

static void fill_mapped_error(tb_error_t *err, const tb_provider_map_t *r,
                              const char *source, int32_t http_status, int64_t retry_after_ms)
{
    tb_error_set(err, r->domain, r->code, "tb_registry_map_response",
                 "%s: HTTP %d%s%s", source, http_status,
                 r->note ? ": " : "", r->note ? r->note : "");
    err->http_status   = http_status;
    err->retryable     = r->retryable;
    if (retry_after_ms > 0)
        err->retry_after_ms = (uint32_t)retry_after_ms;
}

bool tb_registry_map_response(const char *source, int32_t http_status,
                              const char *body, size_t body_len,
                              int64_t retry_after_ms,
                              tb_error_t *err)
{
    if (!source) source = "*";

    /* 2xx — успех (кроме 202, который трактуем как PENDING). */
    if (http_status >= 200 && http_status < 300 && http_status != 202) {
        tb_error_clear(err);
        return false;
    }

    /* 1. Точные правила источника (с паттерном и без). */
    for (size_t i = 0; i < sizeof(tb_builtin_maps) / sizeof(tb_builtin_maps[0]); i++)
        if (rule_matches(&tb_builtin_maps[i], source, http_status, body, body_len) &&
            strcmp(tb_builtin_maps[i].source, source) == 0) {
            fill_mapped_error(err, &tb_builtin_maps[i], source, http_status, retry_after_ms);
            return true;
        }
    /* 2. Пользовательские правила. */
    tb_spin_lock(&g_registry_lock);
    for (size_t i = 0; i < g_custom_count; i++) {
        if (rule_matches(&g_custom_maps[i], source, http_status, body, body_len) &&
            strcmp(g_custom_maps[i].source, source) == 0) {
            tb_provider_map_t hit = g_custom_maps[i];
            tb_spin_unlock(&g_registry_lock);
            fill_mapped_error(err, &hit, source, http_status, retry_after_ms);
            return true;
        }
    }
    tb_spin_unlock(&g_registry_lock);
    /* 3. Общие правила «*». */
    for (size_t i = 0; i < sizeof(tb_builtin_maps) / sizeof(tb_builtin_maps[0]); i++)
        if (rule_matches(&tb_builtin_maps[i], source, http_status, body, body_len)) {
            fill_mapped_error(err, &tb_builtin_maps[i], source, http_status, retry_after_ms);
            return true;
        }

    /* 4. Эвристика по классу статуса. */
    tb_error_t fallback;
    memset(&fallback, 0, sizeof(fallback));
    uint32_t code;
    bool retryable;
    if (http_status == 429)      { code = TB_API_RATE_LIMIT;   retryable = true; }
    else if (http_status == 408) { code = TB_API_TIMEOUT;      retryable = true; }
    else if (http_status == 202) { code = TB_API_PENDING;      retryable = true; }
    else if (http_status >= 500) { code = TB_API_SERVER;       retryable = true; }
    else if (http_status == 401 || http_status == 403)
                                 { code = TB_API_UNAUTHORIZED; retryable = false; }
    else if (http_status == 404) { code = TB_API_NOT_FOUND;    retryable = false; }
    else if (http_status >= 400) { code = TB_API_BAD_REQUEST;  retryable = false; }
    else                         { code = TB_API_NETWORK;      retryable = true; }

    tb_error_set(err, TB_DOMAIN_API, code, "tb_registry_map_response",
                 "%s: HTTP %d (правило по умолчанию)", source, http_status);
    err->http_status = http_status;
    err->retryable   = retryable;
    if (retry_after_ms > 0)
        err->retry_after_ms = (uint32_t)retry_after_ms;
    return true;
}

bool tb_registry_add_source_map(const tb_provider_map_t *rule, tb_error_t *err)
{
    if (!rule || !rule->source) {
        tb_error_set(err, TB_DOMAIN_CORE, TB_CORE_INVALID_ARG, "tb_registry_add_source_map",
                     "rule/source обязателен");
        return false;
    }
    tb_spin_lock(&g_registry_lock);
    if (g_custom_count >= TB_REGISTRY_MAX_CUSTOM) {
        tb_spin_unlock(&g_registry_lock);
        tb_error_set(err, TB_DOMAIN_CORE, TB_CORE_OVERFLOW, "tb_registry_add_source_map",
                     "достигнут лимит пользовательских правил (%d)", TB_REGISTRY_MAX_CUSTOM);
        return false;
    }
    g_custom_maps[g_custom_count++] = *rule;
    tb_spin_unlock(&g_registry_lock);
    return true;
}

size_t tb_registry_map_count(void)
{
    return sizeof(tb_builtin_maps) / sizeof(tb_builtin_maps[0]) + g_custom_count;
}

const tb_provider_map_t *tb_registry_map_at(size_t index)
{
    size_t builtin_n = sizeof(tb_builtin_maps) / sizeof(tb_builtin_maps[0]);
    if (index < builtin_n) return &tb_builtin_maps[index];
    index -= builtin_n;
    if (index < g_custom_count) return &g_custom_maps[index];
    return NULL;
}
