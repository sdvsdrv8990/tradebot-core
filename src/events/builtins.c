#define TB_CURRENT_GROUP TB_G_BUILTINS /* зона ответственности файла */
/* ==========================================================================
 *  tb/events/builtins.c — готовые шаги-шлюзы и наблюдатель журнала.
 * ========================================================================== */
#include "tb/events/builtins.h"
#include "tb/core/error_registry.h"
#include "tb/core/log.h"
#include "tb/core/marker.h"
#include "tb/core/time.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/* ======================================================================
 *  Шаг «rate limit»
 * ====================================================================== */

tb_ev_action_t tb_evmod_ratelimit_fn(tb_evreg_t *r, tb_event_t *ev, void *user)
{
    tb_evmod_ratelimit_t *st = user;
    if (!st || !st->rl)
        return TB_EV_ACT_CHAIN; /* без конфигурации шаг прозрачен */

    uint64_t now = st->now_ms ? st->now_ms() : tb_now_ms();

    if (tb_ratelimit_acquire(st->rl, now)) {
        tb_event_step(r, ev, "granted: токен взят (granted=%" PRIu64
                             ", deferred=%" PRIu64 ")",
                      st->rl->granted, st->rl->deferred);
        return TB_EV_ACT_CHAIN;
    }

    uint64_t wait = tb_ratelimit_next_slot_ms(st->rl, now);
    tb_event_step(r, ev, "deferred: корзина пуста (rate=%u/мин, burst=%u), "
                         "токен через %" PRIu64 " мс",
                  st->rl->rate_per_min, st->rl->burst, wait);
    /* Шлюз на wake обязан ПЕРЕПРОВЕРИТЬСЯ (токен мог забрать другой
     * модуль) — семантика полного повтора, а не продолжения после шлюза. */
    tb_event_retry_at(ev, now + wait);
    return TB_EV_ACT_BLOCK;
}

/* ======================================================================
 *  Шаг «circuit breaker» — контекст probe_at явно (урок t=102)
 * ====================================================================== */

tb_ev_action_t tb_evmod_breaker_fn(tb_evreg_t *r, tb_event_t *ev, void *user)
{
    tb_evmod_breaker_t *st = user;
    if (!st || !st->br)
        return TB_EV_ACT_CHAIN;

    uint64_t now       = st->now_ms ? st->now_ms() : tb_now_ms();
    uint64_t probe_at  = tb_breaker_probe_at_ms(st->br);

    if (tb_breaker_allow(st->br, now)) {
        tb_event_step(r, ev, "allowed: %s, сбоев подряд %u/%u%s",
                      tb_breaker_state_name(st->br),
                      st->br->consecutive_failures, st->br->failure_threshold,
                      st->br->state == TB_BREAKER_HALF_OPEN
                          ? " (проба HALF_OPEN зарезервирована)"
                          : "");
        return TB_EV_ACT_CHAIN;
    }

    if (probe_at == 0) {
        /* HALF_OPEN с занятой пробой: следующая попытка — после
         * результата пробы; перепроверяем с коротким интервалом. */
        uint64_t recheck = st->br->cooldown_ms / 4;
        if (recheck > 500) recheck = 500;
        if (recheck < 50)  recheck = 50;
        tb_event_step(r, ev,
                      "rejected: HALF_OPEN, проба занята (probes=%u/%u) — "
                      "перепроверка через %" PRIu64 " мс",
                      st->br->half_open_probes, st->br->half_open_max_probes,
                      recheck);
        tb_event_retry_at(ev, now + recheck);
        return TB_EV_ACT_BLOCK;
    }

    /* Проба недоступна: след называет все величины — opened_at, cooldown
     * и probe_at. Никакой арифметики «на стороне»: именно здесь проходил
     * баг теста (ждать 100, а не 102 при открытии в t=2). */
    tb_event_step(r, ev,
                  "rejected: OPEN с t=%" PRIu64 ", cooldown=%" PRIu64
                  " -> probe_at=%" PRIu64 " (через %" PRIu64 ")",
                  st->br->opened_at_ms, st->br->cooldown_ms, probe_at,
                  probe_at > now ? probe_at - now : 0);
    /* Полный повтор: на probe_at событие заново пройдёт цепочку, и шаг
     * breaker сам зарезервирует пробу HALF_OPEN (tb_breaker_allow). */
    tb_event_retry_at(ev, probe_at);
    return TB_EV_ACT_BLOCK;
}

/* ======================================================================
 *  Наблюдатель «Event Ledger»
 * ====================================================================== */

static size_t json_escape_into(const char *in, char *out, size_t cap)
{
    size_t n = 0;
    if (!in) {
        if (cap) out[0] = '\0';
        return 0;
    }
    for (const unsigned char *p = (const unsigned char *)in; *p && n + 8 < cap; p++) {
        if (*p == '"' || *p == '\\') {
            out[n++] = '\\';
            out[n++] = (char)*p;
        } else if (*p == '\n') {
            out[n++] = '\\';
            out[n++] = 'n';
        } else if (*p == '\r') {
            out[n++] = '\\';
            out[n++] = 'r';
        } else if (*p == '\t') {
            out[n++] = '\\';
            out[n++] = 't';
        } else if (*p < 0x20) {
            n += (size_t)snprintf(out + n, cap - n, "\\u%04x", *p);
        } else {
            out[n++] = (char)*p;
        }
    }
    out[n] = '\0';
    return n;
}

static void ledger_observer(tb_evreg_t *r, tb_event_t *ev, void *user)
{
    tb_ledger_t *ledger = user;
    if (!ledger || !ev)
        return;

    char trace_esc[2 * TB_EV_TRACE_MAX];
    json_escape_into(ev->trace, trace_esc, sizeof(trace_esc));

    char msg_esc[2 * TB_ERROR_MSG_MAX];
    json_escape_into(ev->err.message, msg_esc, sizeof(msg_esc));

    const tb_error_meta_t *meta = tb_registry_meta(ev->err.domain, ev->err.code);

    char payload[2 * TB_EV_TRACE_MAX + 2 * TB_ERROR_MSG_MAX + 512];
    int n;
    if (tb_error_is_set(&ev->err)) {
        n = snprintf(payload, sizeof(payload),
                     "{\"reg_event\":%llu,\"kind\":%u,\"state\":\"%s\","
                     "\"attempts\":%u,\"steps\":%u,\"wake_at_ms\":%llu,"
                     "\"error\":{\"domain\":\"%s\",\"group\":\"%s\","
                     "\"code\":\"%s\","
                     "\"severity\":\"%s\",\"retryable\":%s,"
                     "\"http_status\":%d,\"retry_after_ms\":%u,\"message\":\"%s\"},"
                     "\"trace\":\"%s\"}",
                     (unsigned long long)ev->id, ev->kind,
                     tb_ev_state_name((tb_ev_state_t)ev->state),
                     ev->attempts, ev->steps, (unsigned long long)ev->wake_at_ms,
                     tb_domain_name(ev->err.domain),
                     meta ? meta->name : "UNKNOWN",
                     tb_severity_name((tb_severity_t)ev->err.severity),
                     ev->err.retryable ? "true" : "false",
                     ev->err.http_status, ev->err.retry_after_ms,
                     msg_esc, trace_esc);
    } else {
        n = snprintf(payload, sizeof(payload),
                     "{\"reg_event\":%llu,\"kind\":%u,\"state\":\"%s\","
                     "\"attempts\":%u,\"steps\":%u,\"wake_at_ms\":%llu,"
                     "\"error\":null,\"trace\":\"%s\"}",
                     (unsigned long long)ev->id, ev->kind,
                     tb_ev_state_name((tb_ev_state_t)ev->state),
                     ev->attempts, ev->steps, (unsigned long long)ev->wake_at_ms,
                     trace_esc);
    }
    if (n < 0)
        return;

    tb_error_t lerr = TB_ERROR_INIT;
    if (tb_ledger_emit(ledger, "event.trace", payload, NULL, NULL, &lerr) != 0)
        TB_LOG_ERROR("events: журнал отклонил запись события #%llu: %s",
                     (unsigned long long)ev->id, lerr.message);

    /* Журнал — долговременная память: живым остаёмся только в live-таблице. */
    tb_event_close(r, ev);
}

int tb_ev_ledger_attach(tb_evreg_t *r, tb_ledger_t *ledger)
{
    if (!r || !ledger)
        return -1;
    return tb_evreg_on_state(r, TB_EV_TERMINAL_MASK, ledger_observer, ledger);
}
