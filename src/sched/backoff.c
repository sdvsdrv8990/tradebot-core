/* ==========================================================================
 *  tb/sched/backoff.c — экспоненциальный backoff + jitter + Retry-After.
 * ========================================================================== */
#include "tb/sched/backoff.h"

#include <inttypes.h>
#include <string.h>

void tb_backoff_init(tb_backoff_t *b, uint64_t base_ms, uint64_t cap_ms,
                     double jitter_frac, uint64_t seed)
{
    if (!b) return;
    memset(b, 0, sizeof(*b));
    b->base_ms     = base_ms ? base_ms : 1;
    b->cap_ms      = cap_ms >= b->base_ms ? cap_ms : b->base_ms;
    b->jitter_frac = jitter_frac < 0 ? 0 : (jitter_frac > 1 ? 1 : jitter_frac);
    b->rng_state   = seed ? seed : 0x9E3779B97F4A7C15ull;
}

uint64_t tb_rand_next(tb_backoff_t *b)
{
    uint64_t x = b->rng_state;
    if (x == 0) x = 0x9E3779B97F4A7C15ull;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    b->rng_state = x;
    return x * 0x2545F4914F6CDD1Dull;
}

uint64_t tb_backoff_delay(tb_backoff_t *b, uint32_t attempt, const tb_error_t *err)
{
    if (!b) return 0;

    /* Неретраебельная ошибка: ожидание не спасает — повтора не будет.
     * Пустой/отсутствующий err — обычный расчёт без ограничений. */
    if (err && tb_error_is_set(err) && !err->retryable)
        return 0;

    if (attempt == 0)
        attempt = 1;

    /* Экспонента с защитой от переполнения: сдвиг не более 16. */
    uint32_t shift = attempt - 1;
    if (shift > 16) shift = 16;
    uint64_t d = b->base_ms << shift;
    if (d > b->cap_ms || d < b->base_ms) /* второй случай — переполнение */
        d = b->cap_ms;

    /* Jitter: равномерно в [d*(1-f), d*(1+f)]. */
    double f = b->jitter_frac;
    if (f > 0) {
        double r = (double)(tb_rand_next(b) >> 11) / 9007199254740992.0; /* [0,1) */
        double lo = (double)d * (1.0 - f);
        double hi = (double)d * (1.0 + f);
        double dj = lo + (hi - lo) * r;
        if (dj < 1.0) dj = 1.0;
        d = (uint64_t)dj;
    }

    /* Retry-After обязателен к учёту: не раньше разрешённого источником. */
    if (err && err->retry_after_ms > 0 && d < err->retry_after_ms)
        d = err->retry_after_ms;

    return d;
}

bool tb_retry_exhausted(const tb_retry_policy_t *policy, uint32_t attempt)
{
    if (!policy) return true;
    if (policy->max_attempts == 0) return attempt > 0; /* повторы запрещены */
    return attempt >= policy->max_attempts;
}
