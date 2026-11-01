/* tests/test_runtime.c — token-bucket и circuit breaker. */
#include "harness.h"
#include "tb/runtime/breaker.h"
#include "tb/runtime/ratelimit.h"

int main(void)
{
    /* ---- Token-bucket: 30/мин, burst 30 ---- */
    tb_ratelimit_t rl;
    tb_ratelimit_init(&rl, 30, 30);
    for (int i = 0; i < 30; i++)
        CHECK(tb_ratelimit_acquire(&rl, 1000));
    CHECK(!tb_ratelimit_acquire(&rl, 1000)); /* 31-й — лимит */
    CHECK(rl.granted == 30 && rl.deferred == 1);

    uint64_t wait = tb_ratelimit_next_slot_ms(&rl, 1000);
    CHECK(wait > 1500 && wait <= 2100);      /* ~2000 мс на токен */

    CHECK(tb_ratelimit_acquire(&rl, 1000 + 2000)); /* пополнилось на 1 */

    /* Burst меньше скорости: 12/мин, burst 4. */
    tb_ratelimit_init(&rl, 12, 4);
    for (int i = 0; i < 4; i++)
        CHECK(tb_ratelimit_acquire(&rl, 500));
    CHECK(!tb_ratelimit_acquire(&rl, 500));
    CHECK(!tb_ratelimit_acquire(&rl, 505));          /* ещё рано */
    CHECK(tb_ratelimit_acquire(&rl, 500 + 5000));    /* +1 токен за 5 с */

    /* ---- Circuit breaker: порог 3, охлаждение 100 мс ---- */
    tb_breaker_t br;
    tb_breaker_init(&br, 3, 100);

    CHECK(tb_breaker_allow(&br, 0));
    tb_breaker_failure(&br, 0);
    tb_breaker_failure(&br, 1);
    CHECK(br.state == TB_BREAKER_CLOSED);
    CHECK(tb_breaker_allow(&br, 2));
    tb_breaker_failure(&br, 2); /* третья подряд */
    CHECK(br.state == TB_BREAKER_OPEN);
    CHECK(!tb_breaker_allow(&br, 50));
    CHECK(br.rejected_total == 1);
    CHECK(tb_breaker_retry_in_ms(&br, 50) == 52); /* открыт в момент 3-й ошибки */
    CHECK_STR_EQ(tb_breaker_state_name(&br), "OPEN");

    /* probe_at — явно, без арифметики на стороне вызывающего:
     * открыт в t=2 при охлаждении 100 — проба с t=102. */
    CHECK(tb_breaker_probe_at_ms(&br) == 102);
    CHECK(!tb_breaker_allow(&br, 101));

    /* Охлаждение прошло (открыт в t=2, порог 100): HALF_OPEN, одна проба. */
    CHECK(tb_breaker_allow(&br, 102));
    CHECK(br.state == TB_BREAKER_HALF_OPEN);
    CHECK(!tb_breaker_allow(&br, 103)); /* проба уже занята */
    tb_breaker_success(&br);
    CHECK(br.state == TB_BREAKER_CLOSED);
    CHECK(br.consecutive_failures == 0);
    CHECK_STR_EQ(tb_breaker_state_name(&br), "CLOSED");

    /* Проба провалилась — снова OPEN (открыт в t=202). */
    tb_breaker_failure(&br, 200);
    tb_breaker_failure(&br, 201);
    tb_breaker_failure(&br, 202);
    CHECK(br.state == TB_BREAKER_OPEN);
    CHECK(tb_breaker_allow(&br, 1000)); /* HALF_OPEN */
    tb_breaker_failure(&br, 1000);
    CHECK(br.state == TB_BREAKER_OPEN);
    CHECK(tb_breaker_retry_in_ms(&br, 1000) == 100);
    CHECK(tb_breaker_probe_at_ms(&br) == 1100); /* повторное открытие в t=1000 */

    /* В состояниях, отличных от OPEN, probe_at не определён. */
    tb_breaker_success(&br);
    tb_breaker_init(&br, 5, 500);
    CHECK(tb_breaker_probe_at_ms(&br) == 0); /* CLOSED */

    /* Успех в CLOSED сбрасывает серию. */
    tb_breaker_init(&br, 2, 50);
    tb_breaker_failure(&br, 0);
    tb_breaker_success(&br);
    tb_breaker_failure(&br, 1);
    CHECK(br.state == TB_BREAKER_CLOSED); /* серия из одного сбоя */

    printf("OK test_runtime\n");
    return 0;
}
