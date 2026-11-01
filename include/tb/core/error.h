/* ==========================================================================
 *  tb/core/error.h — ядро обработки ошибок на всё приложение.
 *
 *  Модель: каждая ошибка — это значение tb_error_t (код + контекст), без
 *  исключений и без errno. Функции ядра возвращают int (0 = успех, -1 = провал)
 *  и заполняют tb_error_t по указателю (может быть NULL — тогда контекст
 *  теряется, остаётся только код возврата).
 *
 *  Домены задают подсистему, код — конкретную причину внутри домена.
 *  Каноническая база кодов (имена, severity, retryable, шаблоны источников
 *  данных) живёт в реестре: tb/core/error_registry.h — «база данных
 *  обработчика». tb_error_set() автоматически подтягивает метаданные оттуда,
 *  поэтому продюсеру достаточно указать домен и код.
 * ========================================================================== */
#ifndef TB_CORE_ERROR_H
#define TB_CORE_ERROR_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "tb/core/ids.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TB_ERROR_MSG_MAX 192   /* сообщение + хвост цепочки контекста */
#define TB_ERROR_LOC_MAX 96    /* file:line источника */

/* ---- Домены (подсистемы). USER — для внешних модулей бота. ---- */
typedef enum {
    TB_DOMAIN_NONE   = 0,
    TB_DOMAIN_CORE   = 1,   /* память, время, конфигурация, базовые утилиты */
    TB_DOMAIN_SCHED  = 2,   /* планировщик задач, таймеры, пул потоков */
    TB_DOMAIN_NET    = 3,   /* транспорт (HTTP — в следующих итерациях) */
    TB_DOMAIN_API    = 4,   /* внешние источники: CoinGecko, GeckoTerminal, GoPlus, DefiLlama */
    TB_DOMAIN_LEDGER = 5,   /* журнал событий */
    TB_DOMAIN_JSON   = 6,   /* сериализация/разбор Envelope */
    TB_DOMAIN_EVENTS = 7,   /* реестр событий: цепочка модулей, live-таблица, прогон */
    TB_DOMAIN_USER   = 64   /* фильтры 1–4, Decision Engine, Execution Engine */
} tb_domain_t;

const char *tb_domain_name(uint8_t domain);

/* ---- Строгость ---- */
typedef enum {
    TB_SEV_INFO  = 0,    /* штатная ситуация, достойная записи */
    TB_SEV_WARN  = 1,    /* деградация, автоловление */
    TB_SEV_ERROR = 2,    /* операция не выполнена */
    TB_SEV_FATAL = 3     /* продолжение невозможно — panic-политика */
} tb_severity_t;

const char *tb_severity_name(tb_severity_t sev);

/* ---- Унифицированные коды домена API (стандартизация ответов источников) ----
 * Точная семантика, severity и retryable — в реестре (error_registry.h). */
typedef enum {
    TB_API_OK           = 0,
    TB_API_RATE_LIMIT   = 1,   /* HTTP 429: повтор обязателен, учитывать Retry-After */
    TB_API_TIMEOUT      = 2,   /* HTTP 408 или таймаут клиента */
    TB_API_NETWORK      = 3,   /* обрыв соединения, DNS и т.п. */
    TB_API_SERVER       = 4,   /* 5xx на стороне источника */
    TB_API_UNAUTHORIZED = 5,   /* 401/403: повторами не исправляется */
    TB_API_NOT_FOUND    = 6,   /* 404: неизвестный токен/пул/сеть */
    TB_API_BAD_REQUEST  = 7,   /* 400/422: например, неверный адрес контракта */
    TB_API_PARSE        = 8,   /* ответ не соответствует ожидаемой схеме */
    TB_API_PENDING      = 9,   /* HTTP 202: фоновая задача поставщика */
    TB_API_CIRCUIT_OPEN = 10,  /* наш circuit breaker разомкнут */
    TB_API_CANCELED     = 11,
    TB_API_UNSUPPORTED  = 12
} tb_api_code_t;

/* ---- Коды остальных доменов ---- */
typedef enum {
    TB_CORE_OK            = 0,
    TB_CORE_NO_MEMORY     = 1,
    TB_CORE_INVALID_ARG   = 2,
    TB_CORE_NOT_INIT      = 3,
    TB_CORE_UNSUPPORTED   = 4,
    TB_CORE_OVERFLOW      = 5,
    TB_CORE_IO            = 6
} tb_core_code_t;

typedef enum {
    TB_SCHED_OK             = 0,
    TB_SCHED_TASK_NOT_FOUND = 1,
    TB_SCHED_SHUTDOWN       = 2,
    TB_SCHED_POOL_CLOSED    = 3,
    TB_SCHED_TOO_MANY_WATCH = 4
} tb_sched_code_t;

typedef enum {
    TB_NET_OK       = 0,
    TB_NET_DNS      = 1,   /* резолв хоста не удался */
    TB_NET_CONNECT  = 2,   /* соединение не установлено (отказ/таймаут connect) */
    TB_NET_TLS      = 3,   /* TLS-рукопожатие или проверка сертификата */
    TB_NET_TIMEOUT  = 4,   /* таймаут чтения/записи */
    TB_NET_BAD_URL  = 5,   /* url не разобран */
    TB_NET_RESPONSE = 6,   /* ответ сервера не соответствует HTTP */
    TB_NET_CLOSED   = 7    /* соединение закрыто до полного ответа */
} tb_net_code_t;

typedef enum {
    TB_LEDGER_OK       = 0,
    TB_LEDGER_IO       = 1,
    TB_LEDGER_BAD_JSON = 2,
    TB_LEDGER_DUP      = 3   /* информационно: дубликат пропущен */
} tb_ledger_code_t;

typedef enum {
    TB_JSON_OK     = 0,
    TB_JSON_PARSE  = 1,
    TB_JSON_SCHEMA = 2,
    TB_JSON_BUFFER = 3
} tb_json_code_t;

typedef enum {
    TB_EV_OK         = 0,
    TB_EV_BAD_HANDLE = 1,  /* неверный реестр/событие/имя шага */
    TB_EV_NO_SLOT    = 2,  /* таблица модулей или live-таблица событий переполнена */
    TB_EV_NO_MODULE  = 3,  /* шаг цепочки с таким именем не найден */
    TB_EV_DUPLICATE  = 4,  /* имя шага уже занято в цепочке */
    TB_EV_BAD_STATE  = 5,  /* операция несовместима с состоянием события */
    TB_EV_RECURSION  = 6,  /* вложенный прогон цепочки запрещён */
    TB_EV_CONTRACT   = 7   /* шаг нарушил контракт (FAIL без заполненной ошибки) */
} tb_events_code_t;

/* ---- Значение ошибки ----
 * Чистое POD-значение: нулевое состояние (TB_ERROR_INIT) означает «нет
 * ошибки», копирование — memcpy, владения кучей нет. Контекст накапливается
 * текстовым следом (tb_error_wrap / tb_error_promote), а не цепочкой
 * указателей — это делает значение безопасным к нулевой инициализации. */
typedef struct tb_error {
    uint8_t   domain;           /* tb_domain_t */
    uint16_t  group;            /* tb_group_id_t: кто сообщил (зона
                                 * ответственности); 0 = не задан */
    uint32_t  code;             /* код внутри домена */
    uint8_t   severity;         /* tb_severity_t (по умолчанию — из реестра) */
    bool      retryable;        /* имеет ли смысл повторять операцию */
    int32_t   http_status;      /* 0 — не применимо */
    uint32_t  retry_after_ms;   /* >0 — источник явно потребовал паузу (Retry-After) */
    char      message[TB_ERROR_MSG_MAX];  /* текст + след контекста */
    char      location[TB_ERROR_LOC_MAX]; /* цепочка file:line через " <- " */
} tb_error_t;

#define TB_ERROR_INIT { 0 }

/* Инициализация / проверка */
void tb_error_clear(tb_error_t *err);
bool tb_error_is_set(const tb_error_t *err);

/* Основной конструктор. Метаданные (severity, retryable) берутся из реестра.
 * location — обычно TB_LOC из макроса. Полностью замещает прежнее
 * состояние err (сообщение и след контекста). Вариант _g несёт группу-
 * владельца (кто сообщил); без группы — TB_G_UNASSIGNED. */
void tb_error_set_g(tb_error_t *err, uint16_t group, uint8_t domain,
                    uint32_t code, const char *location, const char *fmt, ...)
    __attribute__((format(printf, 6, 7)));
void tb_error_set(tb_error_t *err, uint8_t domain, uint32_t code,
                  const char *location, const char *fmt, ...)
    __attribute__((format(printf, 5, 6)));

/* Обогащение контекстом без смены кода: текст добавляется в начало
 * сообщения, location обновляется, группа — на обогатившего (кто
 * пробрасывает вверх, тот и отвечает за кадр). */
void tb_error_wrap_g(tb_error_t *err, uint16_t group, const char *location,
                     const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
void tb_error_wrap(tb_error_t *err, const char *location, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* Поднятие новой ошибки поверх существующей: старое сообщение сохраняется
 * в следе («новое <- прежнее»), домен/код меняются на новые. */
void tb_error_promote_g(tb_error_t *err, uint16_t group, uint8_t domain,
                        uint32_t code, const char *location, const char *fmt, ...)
    __attribute__((format(printf, 6, 7)));
void tb_error_promote(tb_error_t *err, uint8_t domain, uint32_t code,
                      const char *location, const char *fmt, ...)
    __attribute__((format(printf, 5, 6)));

/* Однострочное представление:
 * "[API/TB_API_RATE_LIMIT|429|retry_after=3000ms] сообщение @ loc <- причина" */
size_t tb_error_format(const tb_error_t *err, char *buf, size_t cap);

/* ---- Макросы-шорткаты ---- */
#define TB_STRINGIFY2(x) #x
#define TB_STRINGIFY(x)  TB_STRINGIFY2(x)
#define TB_LOC           __FILE__ ":" TB_STRINGIFY(__LINE__)

/* Группа-владелец текущего файла: объявляется ПЕРВОЙ строкой .c до всех
 * include. Не переопределяется после подключения error.h — поэтому
 * правило: #define TB_CURRENT_GROUP TB_G_... в шапке файла. */
#ifndef TB_CURRENT_GROUP
#define TB_CURRENT_GROUP TB_G_UNASSIGNED
#endif

#define TB_ERROR_SET(err, domain, code, ...) \
    tb_error_set_g((err), TB_CURRENT_GROUP, (domain), (code), TB_LOC,       \
                   __VA_ARGS__)
#define TB_ERROR_FAIL(err, domain, code, ...)                                \
    do {                                                                     \
        TB_ERROR_SET((err), (domain), (code), __VA_ARGS__);                  \
        return -1;                                                           \
    } while (0)
#define TB_ERROR_WRAP(err, ...) \
    tb_error_wrap_g((err), TB_CURRENT_GROUP, TB_LOC, __VA_ARGS__)
#define TB_ERROR_PROMOTE(err, domain, code, ...) \
    tb_error_promote_g((err), TB_CURRENT_GROUP, (domain), (code), TB_LOC,    \
                       __VA_ARGS__)

/* ---- Глобальный обработчик на всё приложение ----
 * Любой компонент может сообщить об ошибке через tb_report(): обработчик
 * считает, логирует и/или кладёт событие в Event Ledger. Обработчик НЕ
 * становится владельцем err. */
typedef void (*tb_error_handler_fn)(const tb_error_t *err, void *user);

tb_error_handler_fn tb_set_error_handler(tb_error_handler_fn handler, void *user);

/* Сообщить ошибку глобальному обработчику (no-op, если не установлен). */
void tb_report(const tb_error_t *err);

/* ---- Политика фатальных ошибок ---- */
typedef void (*tb_panic_fn)(const tb_error_t *err, void *user);

tb_panic_fn tb_set_panic_handler(tb_panic_fn fn, void *user);

/* TB_SEV_FATAL доходит до panic-обработчика; по умолчанию — abort(). */
void tb_panic(const char *location, const char *fmt, ...)
    __attribute__((format(printf, 2, 3), noreturn));

#ifdef __cplusplus
}
#endif
#endif /* TB_CORE_ERROR_H */
