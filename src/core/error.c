#define TB_CURRENT_GROUP TB_G_CORE /* зона ответственности файла */
/* ==========================================================================
 *  tb/core/error.c — реализация ядра обработки ошибок.
 *
 *  tb_error_t — POD-значение без владения кучей: безопасно к нулевой
 *  инициализации, копируется присваиванием. Контекст накапливается
 *  текстовым следом (message/location), а не цепочкой указателей.
 * ========================================================================== */
#include "tb/core/error.h"
#include "tb/core/error_registry.h"
#include "tb/core/log.h"
#include "tb/core/strutil.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void tb_error_detail(struct tb_error *err, uint16_t group, uint16_t cmd,
                     const void *data, size_t len)
{
    if (!err || !data)
        return;
    if (len > TB_DETAIL_DATA_MAX)
        len = TB_DETAIL_DATA_MAX;

    /* details_n — счётчик добавлений: слот = n % MAX, кольцо;
     * заполненные капсулы замещаются по кругу (старейшая первой). */
    uint8_t slot = err->details_n % TB_ERROR_DETAILS_MAX;
    if (err->details_n < 0xff)
        err->details_n++;

    tb_detail_t *d = &err->details[slot];
    d->group = group;
    d->cmd = cmd;
    memset(d->data, 0, sizeof(d->data));
    memcpy(d->data, data, len);
    d->data_len = (uint8_t)len;
}

/* ---- Глобальный обработчик ---- */
static tb_error_handler_fn g_handler;
static void               *g_handler_user;
static tb_panic_fn         g_panic;
static void               *g_panic_user;

const char *tb_domain_name(uint8_t domain)
{
    switch (domain) {
    case TB_DOMAIN_CORE:   return "CORE";
    case TB_DOMAIN_SCHED:  return "SCHED";
    case TB_DOMAIN_NET:    return "NET";
    case TB_DOMAIN_API:    return "API";
    case TB_DOMAIN_LEDGER: return "LEDGER";
    case TB_DOMAIN_JSON:   return "JSON";
    case TB_DOMAIN_EVENTS: return "EVENTS";
    case TB_DOMAIN_USER:   return "USER";
    default:               return "NONE";
    }
}

const char *tb_severity_name(tb_severity_t sev)
{
    switch (sev) {
    case TB_SEV_INFO:  return "INFO";
    case TB_SEV_WARN:  return "WARN";
    case TB_SEV_ERROR: return "ERROR";
    case TB_SEV_FATAL: return "FATAL";
    default:           return "?";
    }
}

void tb_error_clear(tb_error_t *err)
{
    if (!err) return;
    memset(err, 0, sizeof(*err));
}

bool tb_error_is_set(const tb_error_t *err)
{
    return err && (err->domain != TB_DOMAIN_NONE || err->code != 0);
}

static void error_setv(tb_error_t *err, uint16_t group, uint8_t domain,
                       uint32_t code, const char *location, const char *fmt,
                       va_list ap)
{
    tb_error_t fresh;
    memset(&fresh, 0, sizeof(fresh));
    fresh.domain = domain;
    fresh.group  = group;
    fresh.code   = code;
    tb_registry_decorate(&fresh);
    if (fmt) {
        vsnprintf(fresh.message, sizeof(fresh.message), fmt, ap);
        tb_utf8_fit(fresh.message, sizeof(fresh.message));
    }
    if (location)
        snprintf(fresh.location, sizeof(fresh.location), "%s", location);
    *err = fresh; /* POD: присваивание целиком, без владения */
}

void tb_error_set_g(tb_error_t *err, uint16_t group, uint8_t domain,
                    uint32_t code, const char *location, const char *fmt, ...)
{
    va_list ap;
    if (!err) return;
    va_start(ap, fmt);
    error_setv(err, group, domain, code, location, fmt, ap);
    va_end(ap);
}

void tb_error_set(tb_error_t *err, uint8_t domain, uint32_t code,
                  const char *location, const char *fmt, ...)
{
    va_list ap;
    if (!err) return;
    va_start(ap, fmt);
    error_setv(err, TB_G_UNASSIGNED, domain, code, location, fmt, ap);
    va_end(ap);
}

/* Обогащение: addend в начало сообщения, location в начало цепочки. */
static void error_wrapv(tb_error_t *err, const char *location, const char *fmt,
                        va_list ap)
{
    char addend[TB_ERROR_MSG_MAX];
    vsnprintf(addend, sizeof(addend), fmt, ap);
    tb_utf8_fit(addend, sizeof(addend));

    char orig[TB_ERROR_MSG_MAX];
    memcpy(orig, err->message, sizeof(orig));

    if (orig[0] != '\0')
        snprintf(err->message, sizeof(err->message), "%s: %s", addend, orig);
    else
        snprintf(err->message, sizeof(err->message), "%s", addend);
    tb_utf8_fit(err->message, sizeof(err->message));

    if (location) {
        char origloc[TB_ERROR_LOC_MAX];
        memcpy(origloc, err->location, sizeof(origloc));
        if (origloc[0] != '\0')
            snprintf(err->location, sizeof(err->location), "%s <- %s",
                     location, origloc);
        else
            snprintf(err->location, sizeof(err->location), "%s", location);
    }
}

void tb_error_wrap_g(tb_error_t *err, uint16_t group, const char *location,
                     const char *fmt, ...)
{
    va_list ap;
    if (!err || !tb_error_is_set(err)) return;
    err->group = group; /* обогативший отвечает за кадр */
    va_start(ap, fmt);
    error_wrapv(err, location, fmt, ap);
    va_end(ap);
}

void tb_error_wrap(tb_error_t *err, const char *location, const char *fmt, ...)
{
    va_list ap;
    if (!err || !tb_error_is_set(err)) return;
    va_start(ap, fmt);
    error_wrapv(err, location, fmt, ap);
    va_end(ap);
}

static void error_promotev(tb_error_t *err, uint16_t group, uint8_t domain,
                           uint32_t code, const char *location, const char *fmt,
                           va_list ap)
{
    if (!err) return;

    char orig[TB_ERROR_MSG_MAX];
    char origloc[TB_ERROR_LOC_MAX];
    tb_detail_t orig_details[TB_ERROR_DETAILS_MAX];
    uint8_t orig_details_n = err->details_n;
    memcpy(orig, err->message, sizeof(orig));
    memcpy(origloc, err->location, sizeof(origloc));
    memcpy(orig_details, err->details, sizeof(orig_details));

    error_setv(err, group, domain, code, location, fmt, ap);
    /* происхождение не теряется: капсулы наследуются */
    memcpy(err->details, orig_details, sizeof(orig_details));
    err->details_n = orig_details_n;

    /* Прежнее сообщение сохраняется следом. */
    if (orig[0] != '\0') {
        char fresh[TB_ERROR_MSG_MAX];
        memcpy(fresh, err->message, sizeof(fresh));
        if (fresh[0] != '\0')
            snprintf(err->message, sizeof(err->message), "%s <- %s", fresh,
                     orig);
        else
            snprintf(err->message, sizeof(err->message), "%s", orig);
        tb_utf8_fit(err->message, sizeof(err->message));
    }
    if (origloc[0] != '\0') {
        char freshloc[TB_ERROR_LOC_MAX];
        memcpy(freshloc, err->location, sizeof(freshloc));
        if (freshloc[0] != '\0')
            snprintf(err->location, sizeof(err->location), "%s <- %s",
                     freshloc, origloc);
        else
            snprintf(err->location, sizeof(err->location), "%s", origloc);
    }
}

void tb_error_promote_g(tb_error_t *err, uint16_t group, uint8_t domain,
                        uint32_t code, const char *location, const char *fmt,
                        ...)
{
    va_list ap;
    va_start(ap, fmt);
    error_promotev(err, group, domain, code, location, fmt, ap);
    va_end(ap);
}

void tb_error_promote(tb_error_t *err, uint8_t domain, uint32_t code,
                      const char *location, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    error_promotev(err, TB_G_UNASSIGNED, domain, code, location, fmt, ap);
    va_end(ap);
}

size_t tb_error_format(const tb_error_t *err, char *buf, size_t cap)
{
    if (!err || !buf || cap == 0) return 0;

    size_t n = 0;
#define TB_APPEND(...) do {                                                   \
        int rc_ = snprintf(buf + n, cap - n, __VA_ARGS__);                     \
        if (rc_ < 0) return 0;                                                \
        n += ((size_t)rc_ < cap - n) ? (size_t)rc_ : (cap - n - 1);            \
    } while (0)

    const tb_error_meta_t *meta = tb_registry_meta(err->domain, err->code);
    if (err->group != TB_G_UNASSIGNED)
        TB_APPEND("[%s:%s/%s", tb_domain_name(err->domain),
                  tb_group_name(err->group), meta ? meta->name : "?");
    else
        TB_APPEND("[%s/%s", tb_domain_name(err->domain),
                  meta ? meta->name : "?");
    if (err->http_status > 0)
        TB_APPEND("|HTTP %d", err->http_status);
    if (err->retry_after_ms > 0)
        TB_APPEND("|retry_after=%ums", err->retry_after_ms);
    TB_APPEND("|%s", err->retryable ? "retryable" : "permanent");
    TB_APPEND("] %s", err->message[0] ? err->message : "-");
    if (err->location[0])
        TB_APPEND(" @ %s", err->location);
#undef TB_APPEND
    tb_utf8_fit(buf, cap); /* итоговая строка могла оборвать символ */
    return n;
}

/* ---- Глобальный обработчик ---- */

tb_error_handler_fn tb_set_error_handler(tb_error_handler_fn handler, void *user)
{
    tb_error_handler_fn prev = g_handler;
    g_handler      = handler;
    g_handler_user = user;
    return prev;
}

void tb_report(const tb_error_t *err)
{
    if (!err || !g_handler) return;
    g_handler(err, g_handler_user);
    if (err->severity == TB_SEV_FATAL)
        tb_panic(err->location, "%s", err->message);
}

tb_panic_fn tb_set_panic_handler(tb_panic_fn fn, void *user)
{
    tb_panic_fn prev = g_panic;
    g_panic      = fn;
    g_panic_user = user;
    return prev;
}

void tb_panic(const char *location, const char *fmt, ...)
{
    char msg[TB_ERROR_MSG_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    tb_utf8_fit(msg, sizeof(msg));

    tb_error_t err = TB_ERROR_INIT;
    err.domain    = TB_DOMAIN_CORE;
    err.code      = TB_CORE_UNSUPPORTED;
    err.severity  = TB_SEV_FATAL;
    err.retryable = false;
    snprintf(err.message, sizeof(err.message), "FATAL: %s", msg);
    tb_utf8_fit(err.message, sizeof(err.message));
    if (location)
        snprintf(err.location, sizeof(err.location), "%s", location);

    if (g_panic) {
        g_panic(&err, g_panic_user);
        abort(); /* обработчик не остановил выполнение — страховка */
    }
    TB_LOG(TB_LOG_FATAL, "panic: %s (%s)", msg, location ? location : "?");
    abort();
}
