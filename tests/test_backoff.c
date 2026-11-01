/* tests/test_backoff.c — экспоненциальный backoff + jitter + Retry-After. */
#include "harness.h"
#include "tb/sched/backoff.h"

int main(void)
{
    tb_backoff_t b;
    tb_backoff_init(&b, 1000, 8000, 0.25, 42);

    /* Попытка 1: [750, 1250]. */
    tb_error_t err = TB_ERROR_INIT;
    tb_error_clear(&err);
    for (int i = 0; i < 200; i++) {
        uint64_t d = tb_backoff_delay(&b, 1, &err);
        CHECK(d >= 750 && d <= 1250);
    }
    /* Попытка 2: [1500, 2500]; попытка 3: [3000, 5000]. */
    for (int i = 0; i < 200; i++) {
        uint64_t d = tb_backoff_delay(&b, 2, &err);
        CHECK(d >= 1500 && d <= 2500);
        d = tb_backoff_delay(&b, 3, &err);
        CHECK(d >= 3000 && d <= 5000);
    }
    /* Попытка 5: база 16000 упирается в потолок 8000 -> [6000, 10000]. */
    for (int i = 0; i < 200; i++) {
        uint64_t d = tb_backoff_delay(&b, 5, &err);
        CHECK(d >= 6000 && d <= 10000);
    }
    /* Попытка 30: потолок держится. */
    uint64_t d = tb_backoff_delay(&b, 30, &err);
    CHECK(d >= 6000 && d <= 10000);

    /* Retry-After доминирует: не раньше разрешённого. */
    tb_error_set(&err, TB_DOMAIN_API, TB_API_RATE_LIMIT, "t", "429");
    err.retryable = true;
    err.retry_after_ms = 5000;
    for (int i = 0; i < 100; i++) {
        d = tb_backoff_delay(&b, 1, &err); /* без RA было бы [750,1250] */
        CHECK(d >= 5000);
    }
    /* Retry-After меньше расчёта — не мешает. */
    err.retry_after_ms = 100;
    d = tb_backoff_delay(&b, 3, &err);
    CHECK(d >= 3000 && d <= 5000);

    /* Неретраебельная ошибка: повтора нет. */
    tb_error_set(&err, TB_DOMAIN_API, TB_API_UNAUTHORIZED, "t", "401");
    CHECK(tb_backoff_delay(&b, 2, &err) == 0);

    /* Детерминизм сидa. */
    tb_backoff_t x, y;
    tb_backoff_init(&x, 100, 1000, 0.3, 7);
    tb_backoff_init(&y, 100, 1000, 0.3, 7);
    for (int i = 0; i < 50; i++)
        CHECK(tb_backoff_delay(&x, 3, NULL) == tb_backoff_delay(&y, 3, NULL));

    /* Политика повторов. */
    tb_retry_policy_t p = tb_retry_policy(3, 100, 1000, 0.1);
    CHECK(!tb_retry_exhausted(&p, 0));
    CHECK(!tb_retry_exhausted(&p, 2));
    CHECK(tb_retry_exhausted(&p, 3));
    CHECK(!tb_retry_exhausted(&TB_RETRY_NONE, 0));
    CHECK(tb_retry_exhausted(&TB_RETRY_NONE, 1));

    printf("OK test_backoff\n");
    return 0;
}
