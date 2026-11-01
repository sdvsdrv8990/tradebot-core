/* ==========================================================================
 *  tb/runtime/breaker.c — circuit breaker.
 * ========================================================================== */
#include "tb/runtime/breaker.h"

#include <string.h>

void tb_breaker_init(tb_breaker_t *b, uint32_t failure_threshold, uint64_t cooldown_ms)
{
    if (!b) return;
    memset(b, 0, sizeof(*b));
    b->failure_threshold    = failure_threshold ? failure_threshold : 1;
    b->cooldown_ms          = cooldown_ms;
    b->state                = TB_BREAKER_CLOSED;
    b->half_open_max_probes = 1;
}

const char *tb_breaker_state_name(const tb_breaker_t *b)
{
    if (!b) return "?";
    switch (b->state) {
    case TB_BREAKER_CLOSED:    return "CLOSED";
    case TB_BREAKER_OPEN:      return "OPEN";
    case TB_BREAKER_HALF_OPEN: return "HALF_OPEN";
    }
    return "?";
}

static void to_open(tb_breaker_t *b, uint64_t now_ms)
{
    b->state     = TB_BREAKER_OPEN;
    b->opened_at_ms = now_ms;
    b->half_open_probes = 0;
    b->transitions++;
}

bool tb_breaker_allow(tb_breaker_t *b, uint64_t now_ms)
{
    if (!b) return false;
    switch (b->state) {
    case TB_BREAKER_CLOSED:
        return true;
    case TB_BREAKER_OPEN:
        if (now_ms - b->opened_at_ms >= b->cooldown_ms) {
            b->state = TB_BREAKER_HALF_OPEN;
            b->half_open_probes = 0;
            b->transitions++;
            /* fallthrough: резервируем пробу */
        } else {
            b->rejected_total++;
            return false;
        }
        /* FALLTHRU */
    case TB_BREAKER_HALF_OPEN:
    default:
        if (b->half_open_probes < b->half_open_max_probes) {
            b->half_open_probes++;
            return true;
        }
        b->rejected_total++;
        return false;
    }
}

void tb_breaker_success(tb_breaker_t *b)
{
    if (!b) return;
    b->consecutive_failures = 0;
    if (b->state == TB_BREAKER_HALF_OPEN) {
        b->state = TB_BREAKER_CLOSED;
        b->transitions++;
    }
}

void tb_breaker_failure(tb_breaker_t *b, uint64_t now_ms)
{
    if (!b) return;
    if (b->state == TB_BREAKER_HALF_OPEN) {
        to_open(b, now_ms); /* проба провалилась — снова разомкнут */
        return;
    }
    if (++b->consecutive_failures >= b->failure_threshold)
        to_open(b, now_ms);
}

uint64_t tb_breaker_retry_in_ms(const tb_breaker_t *b, uint64_t now_ms)
{
    if (!b) return 0;
    if (b->state != TB_BREAKER_OPEN) return 0;
    uint64_t elapsed = now_ms - b->opened_at_ms;
    return elapsed >= b->cooldown_ms ? 0 : b->cooldown_ms - elapsed;
}

uint64_t tb_breaker_probe_at_ms(const tb_breaker_t *b)
{
    if (!b || b->state != TB_BREAKER_OPEN) return 0;
    return b->opened_at_ms + b->cooldown_ms;
}
