/* ==========================================================================
 *  tb/runtime/ratelimit.c — token-bucket.
 * ========================================================================== */
#include "tb/runtime/ratelimit.h"

#include <string.h>

void tb_ratelimit_init(tb_ratelimit_t *rl, uint32_t rate_per_min, uint32_t burst)
{
    if (!rl) return;
    memset(rl, 0, sizeof(*rl));
    rl->rate_per_min = rate_per_min ? rate_per_min : 1;
    rl->burst        = burst ? burst : 1;
    rl->tokens       = (double)rl->burst;
    rl->last_ms      = 0;
}

static void refill(tb_ratelimit_t *rl, uint64_t now_ms)
{
    if (now_ms <= rl->last_ms) return;
    uint64_t elapsed = now_ms - rl->last_ms;
    /* rate_per_min за минуту -> за миллисекунду (в double для точности). */
    double add = (double)elapsed * (double)rl->rate_per_min / 60000.0;
    rl->tokens += add;
    if (rl->tokens > (double)rl->burst)
        rl->tokens = (double)rl->burst;
    rl->last_ms = now_ms;
}

bool tb_ratelimit_acquire(tb_ratelimit_t *rl, uint64_t now_ms)
{
    if (!rl) return false;
    refill(rl, now_ms);
    if (rl->tokens >= 1.0) {
        rl->tokens -= 1.0;
        rl->granted++;
        return true;
    }
    rl->deferred++;
    return false;
}

uint64_t tb_ratelimit_next_slot_ms(const tb_ratelimit_t *rl, uint64_t now_ms)
{
    if (!rl) return 0;
    /* Локальная копия: функция константная, мутировать нельзя. */
    tb_ratelimit_t tmp = *rl;
    if (tmp.last_ms == 0) tmp.last_ms = now_ms;
    /* Сколько не хватает до целого токена. */
    refill(&tmp, now_ms);
    if (tmp.tokens >= 1.0) return 0;
    double deficit = 1.0 - tmp.tokens;
    double ms_per_token = 60000.0 / (double)tmp.rate_per_min;
    uint64_t wait = (uint64_t)(deficit * ms_per_token) + 1;
    return wait ? wait : 1;
}
