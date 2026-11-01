/* ==========================================================================
 *  tests/test_backend_dex.c — DEX-адаптер на replay-транспорте:
 *  арифметика многохоповых котировок, ABI-calldata, маппинг ошибок RPC,
 *  shadow-сессии submit/poll, ёмкость, slippage.
 * ========================================================================== */
#include <stdio.h>
#include <string.h>

#include "harness.h"
#include "tb/core/error.h"
#include "tb/core/time.h"
#include "tb/exec/backend_dex.h"
#include "tb/net/http.h"

static tb_addr_t A(const char *hex)
{
    tb_addr_t a;
    CHECK(tb_addr_from_hex(&a, hex, NULL) == 0);
    return a;
}

static tb_addr_t WETH = { { 0xc0, 0x2a, 0xaa, 0x39, 0xb2, 0x23, 0xfe, 0x8d,
                            0x0a, 0x0e, 0x5c, 0x4f, 0x27, 0xea, 0xd9, 0x08,
                            0x3c, 0x75, 0x6c, 0xc2 } };
static tb_addr_t USDC = { { 0xa0, 0xb8, 0x69, 0x91, 0xc6, 0x21, 0x8b, 0x36,
                            0xc1, 0xd1, 0x9d, 0x4a, 0x2e, 0x9e, 0xb0, 0xce,
                            0x36, 0x06, 0xeb, 0x48 } };
static tb_addr_t POOL_A = { { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11,
                              0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11,
                              0x11, 0x11, 0x11, 0x11 } };
static tb_addr_t POOL_B = { { 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22,
                              0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22,
                              0x22, 0x22, 0x22, 0x22 } };
static tb_addr_t QUOTER = { { 0x61, 0xff, 0xe0, 0x14, 0xba, 0x17, 0x98, 0x9e,
                              0x74, 0x3c, 0x5f, 0x6c, 0xb2, 0x1b, 0xf9, 0x69,
                              0x75, 0x30, 0xb2, 0x1e } };

static tb_u256_t dec(const char *s)
{
    tb_u256_t v;
    CHECK(tb_u256_from_dec(&v, s, strlen(s), NULL) == 0);
    return v;
}

static void mk_cfg(tb_exec_dex_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    strcpy(cfg->venues[0].name, "uni-test");
    strcpy(cfg->venues[0].rpc_url, "https://rpc.test/x");
    cfg->venues[0].quoter = QUOTER;
    cfg->venues[0].pools[0] = POOL_A;
    cfg->venues[0].pools[1] = POOL_B;
    cfg->venues[0].pools_n = 2;
    cfg->venues_n = 1;
    cfg->fill_delay_min_ms = 200;
    cfg->fill_delay_max_ms = 200;
    cfg->seed = 7;
}

/* Ответ-результат: слово 0 = value, слова 1..3 = 0. */
static const char *rpc_ok(tb_u256_t v)
{
    static char buf[300];
    char hex[131] = "0x";
    int n = 2;
    for (int w = 3; w >= 0; w--) {
        uint64_t word = (w == 0) ? v.w[0] : 0;
        for (int b = 7; b >= 0; b--)
            n += snprintf(hex + n, 3, "%02x",
                          (unsigned)((word >> (8 * b)) & 0xff));
    }
    hex[n] = '\0';
    snprintf(buf, sizeof(buf),
             "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":\"%s\"}", hex);
    return buf;
}

static const char *rpc_err(void)
{
    return "{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"code\":3,"
           "\"message\":\"execution reverted\"}}";
}

static void test_quote_multihop(void)
{
    tb_error_t err = TB_ERROR_INIT;
    tb_exec_dex_cfg_t cfg;
    mk_cfg(&cfg);

    /* хоп1: 1e15 -> 2253600 (USDC, 6 dec), хоп2: 2253600 -> 1001234567890123 */
    tb_u256_t q1 = dec("2253600"), q2 = dec("1001234567890123");
    const char *body1 = rpc_ok(q1), *body2 = rpc_ok(q2);

    tb_http_replay_step_t steps[] = {
        { .status = 200, .body = body1 },
        { .status = 200, .body = body2 },
    };
    tb_http_t *http = tb_http_replay_create(steps, 2, &err);
    CHECK(http != NULL);

    tb_exec_backend_t b;
    CHECK(tb_exec_dex_init(&b, http, &cfg, &err) == 0);
    CHECK_STR_EQ(b.name, "dex-rpc");

    /* двуххоповый маршрут WETH -> USDC -> WETH, пулы A и B */
    tb_addr_t toks[3] = { WETH, USDC, WETH };
    uint32_t fees[2] = { 3000, 500 };
    tb_route_t r = tb_route_encode(toks, fees, 3, &err);
    tb_route_set_pool(&r, 0, &POOL_A);
    tb_route_set_pool(&r, 1, &POOL_B);

    tb_u256_t out, in0 = dec("1000000000000000");
    CHECK(b.quote(&b, &r, &in0, tb_now_ms(), &out, &err) == 0);
    CHECK(tb_u256_cmp(&out, &q2) == 0); /* выход = результат последнего хопа */

    const tb_exec_dex_stats_t *st = tb_exec_dex_stats(&b);
    CHECK(st->quotes == 1 && st->rpc_calls == 2 && st->rpc_errors == 0);

    b.destroy(&b);
    tb_http_destroy(http);
}

static void test_quote_errors(void)
{
    tb_error_t err = TB_ERROR_INIT;
    tb_exec_dex_cfg_t cfg;
    mk_cfg(&cfg);

    tb_route_t r = tb_route_single(&WETH, &USDC, 3000, &err);
    tb_route_set_pool(&r, 0, &POOL_A);
    tb_u256_t out;

    /* 1) execution reverted -> INSUFFICIENT_LIQUIDITY, не ретраебельно */
    {
        tb_http_replay_step_t steps[] = {
            { .status = 200, .body = rpc_err() },
        };
        tb_http_t *http = tb_http_replay_create(steps, 1, &err);
        tb_exec_backend_t b;
        CHECK(tb_exec_dex_init(&b, http, &cfg, &err) == 0);
            tb_u256_t in1 = dec("1000000");
    CHECK(b.quote(&b, &r, &in1, tb_now_ms(), &out, &err) != 0);
        CHECK(err.domain == TB_DOMAIN_USER);
        CHECK(err.code == TB_EXEC_INSUFFICIENT_LIQUIDITY);
        CHECK(!err.retryable);
        b.destroy(&b);
        tb_http_destroy(http);
    }

    /* 2) HTTP 429 -> BACKEND, ретраебельно, Retry-After проходит */
    {
        tb_error_t e2 = TB_ERROR_INIT;
        tb_http_replay_step_t steps[] = {
            { .status = 429, .retry_after_ms = 1500 },
        };
        tb_http_t *http = tb_http_replay_create(steps, 1, &err);
        tb_exec_backend_t b;
        CHECK(tb_exec_dex_init(&b, http, &cfg, &err) == 0);
                tb_u256_t in2 = dec("1000000");
        CHECK(b.quote(&b, &r, &in2, tb_now_ms(), &out, &e2) != 0);
        CHECK(e2.domain == TB_DOMAIN_USER && e2.code == TB_EXEC_BACKEND);
        CHECK(e2.retryable);
        CHECK(e2.retry_after_ms == 1500);
        b.destroy(&b);
        tb_http_destroy(http);
    }

    /* 3) нулевой result -> INSUFFICIENT_LIQUIDITY */
    {
        tb_error_t e3 = TB_ERROR_INIT;
        tb_http_replay_step_t steps[] = {
            { .status = 200, .body = "{\"result\":\"0x\"}" },
        };
        tb_http_t *http = tb_http_replay_create(steps, 1, &err);
        tb_exec_backend_t b;
        CHECK(tb_exec_dex_init(&b, http, &cfg, &err) == 0);
                tb_u256_t in3 = dec("1000000");
        CHECK(b.quote(&b, &r, &in3, tb_now_ms(), &out, &e3) != 0);
        CHECK(e3.code == TB_EXEC_INSUFFICIENT_LIQUIDITY);
        b.destroy(&b);
        tb_http_destroy(http);
    }

    /* 4) пул не приписан бирже -> BACKEND */
    {
        tb_error_t e4 = TB_ERROR_INIT;
        tb_route_t r2 = tb_route_single(&WETH, &USDC, 3000, &err);
        /* пул не задан — нулевой адрес */
        tb_exec_backend_t b;
        CHECK(tb_exec_dex_init(&b, NULL, &cfg, &e4) != 0); /* http NULL */
        tb_http_replay_step_t steps[] = { { .status = 200, .body = "{}" } };
        tb_http_t *http = tb_http_replay_create(steps, 1, &err);
        CHECK(tb_exec_dex_init(&b, http, &cfg, &e4) == 0);
                tb_u256_t in4 = dec("1000000");
        CHECK(b.quote(&b, &r2, &in4, tb_now_ms(), &out, &e4) != 0);
        CHECK(e4.code == TB_EXEC_BACKEND);
        b.destroy(&b);
        tb_http_destroy(http);
    }
}

static void test_submit_poll(void)
{
    tb_error_t err = TB_ERROR_INIT;
    tb_exec_dex_cfg_t cfg;
    mk_cfg(&cfg);

    tb_u256_t q1 = dec("2253600");
    const char *body1 = rpc_ok(q1);
    /* пере-котировка на submit + poll не ходит в сеть */
    tb_http_replay_step_t steps[] = {
        { .status = 200, .body = body1 }, /* quote */
        { .status = 200, .body = body1 }, /* submit: re-quote */
    };
    tb_http_t *http = tb_http_replay_create(steps, 2, &err);
    tb_exec_backend_t b;
    CHECK(tb_exec_dex_init(&b, http, &cfg, &err) == 0);

    tb_route_t r = tb_route_single(&WETH, &USDC, 3000, &err);
    tb_route_set_pool(&r, 0, &POOL_A);

    tb_u256_t in = dec("1000000000000000");
    tb_u256_t min_ok = dec("2250000");
    tb_u256_t min_too_high = dec("99999999");
    uint64_t tx = 0;
    tb_u256_t out;
    uint64_t now = tb_now_ms();

    /* slippage: котировка ниже минимума — отказ, как реверт роутера */
    tb_error_t e1 = TB_ERROR_INIT;
    CHECK(b.submit(&b, &r, &in, &min_too_high, 0, now, &tx, &out,
                   &e1) == -1);
    CHECK(e1.code == TB_EXEC_SLIPPAGE);
    CHECK(tx == 0);

    /* штатный submit: SUBMITTED + сессия */
    tb_error_t e2 = TB_ERROR_INIT;
    CHECK(b.submit(&b, &r, &in, &min_ok, 0, now, &tx, &out, &e2) == 1);
    CHECK(tx != 0);
    CHECK(tb_u256_cmp(&out, &q1) == 0);

    /* ранний poll — ещё в работе */
    CHECK(b.poll(&b, tx, now + 100, &out, &e2) == 1);
    /* после задержки — заполнение по цене submit-котировки */
    CHECK(b.poll(&b, tx, now + 500, &out, &e2) == 0);
    CHECK(tb_u256_cmp(&out, &q1) == 0);
    /* сессия погашена: повтор — неизвестная сессия */
    CHECK(b.poll(&b, tx, now + 900, &out, &e2) == -1);
    CHECK(e2.code == TB_EXEC_BACKEND);

    const tb_exec_dex_stats_t *st = tb_exec_dex_stats(&b);
    CHECK(st->submits == 2 && st->fills == 1);

    b.destroy(&b);
    tb_http_destroy(http);
}

static void test_capacity(void)
{
    tb_error_t err = TB_ERROR_INIT;
    tb_exec_dex_cfg_t cfg;
    mk_cfg(&cfg);

    tb_u256_t q1 = dec("1000");
    const char *body1 = rpc_ok(q1);
    tb_http_replay_step_t steps[] = {
        { .status = 200, .body = body1 },
    };
    tb_http_t *http = tb_http_replay_create(steps, 1, &err);
    tb_exec_backend_t b;
    CHECK(tb_exec_dex_init(&b, http, &cfg, &err) == 0);

    tb_route_t r = tb_route_single(&WETH, &USDC, 3000, &err);
    tb_route_set_pool(&r, 0, &POOL_A);
    tb_u256_t in = dec("1000000");

    uint64_t tx = 0, now = tb_now_ms();
    tb_u256_t out;
    tb_error_t e = TB_ERROR_INIT;
    int i;
    for (i = 0; i < TB_DEX_MAX_SESSIONS; i++) {
        tb_error_t ei = TB_ERROR_INIT;
        CHECK(b.submit(&b, &r, &in, NULL, 0, now, &tx, &out, &ei) == 1);
    }
    CHECK(b.submit(&b, &r, &in, NULL, 0, now, &tx, &out, &e) == -1);
    CHECK(e.code == TB_EXEC_CAPACITY);

    b.destroy(&b);
    tb_http_destroy(http);
}

int main(void)
{
    test_quote_multihop();
    test_quote_errors();
    test_submit_poll();
    test_capacity();
    printf("OK test_backend_dex\n");
    return 0;
}
