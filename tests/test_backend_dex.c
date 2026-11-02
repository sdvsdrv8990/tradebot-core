/* ==========================================================================
 *  tests/test_backend_dex.c — DEX-адаптер на replay-транспорте:
 *  арифметика многохоповых котировок, ABI-calldata, маппинг ошибок RPC,
 *  shadow-сессии submit/poll, ёмкость, slippage; проверка пулов на цепи
 *  (В4: getPool), symbol/decimals (В5), eth_blockNumber (В6), маршрут
 *  через две биржи (В8).
 * ========================================================================== */
#include <stdio.h>
#include <string.h>

#include "harness.h"
#include "tb/core/error.h"
#include "tb/core/keccak.h"
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

/* ---- В4/В5/В6/В8: стартерные вызовы и межбиржевые маршруты ---- */

static const char *addr_word(const tb_addr_t *a)
{
    /* слово с адресом, выровненным вправо */
    static char buf[69];
    char hex[43];
    tb_addr_to_hex(a, hex, sizeof(hex));
    snprintf(buf, sizeof(buf), "0x000000000000000000000000%s", hex + 2);
    return buf;
}

/* ABI-строка: offset 0x20 + len + данные, выровненные в слово */
static const char *abi_string(const char *s)
{
    static char buf[200];
    size_t n = strlen(s);
    buf[0] = '\0';
    strcat(buf, "0x");
    for (int i = 0; i < 62; i++)
        strcat(buf, "0");
    strcat(buf, "20");
    char lenw[65];
    snprintf(lenw, sizeof(lenw), "%064zx", n);
    strcat(buf, lenw);
    char data[65];
    size_t o = 0;
    for (size_t i = 0; i < 32; i++) {
        if (i < n)
            o += (size_t)snprintf(data + o, sizeof(data) - o, "%02x",
                                  (unsigned char)s[i]);
        else
            o += (size_t)snprintf(data + o, sizeof(data) - o, "00");
    }
    strcat(buf, data);
    return buf;
}

/* Селекторы сверены keccak на месте (как 0xc6a5026a для Quoter). */
static void test_selectors_on_site(void)
{
    uint8_t sel[4];
    tb_keccak_selector("quoteExactInputSingle((address,address,uint256,"
                       "uint24,uint160))",
                       sel);
    CHECK(sel[0] == 0xc6 && sel[1] == 0xa5 && sel[2] == 0x02
          && sel[3] == 0x6a);
    tb_keccak_selector("getPool(address,address,uint24)", sel);
    CHECK(sel[0] == 0x16 && sel[1] == 0x98 && sel[2] == 0xee
          && sel[3] == 0x82);
    tb_keccak_selector("getPair(address,address)", sel);
    CHECK(sel[0] == 0xe6 && sel[1] == 0xa4 && sel[2] == 0x39
          && sel[3] == 0x05);
    tb_keccak_selector("getAmountsOut(uint256,address[])", sel);
    CHECK(sel[0] == 0xd0 && sel[1] == 0x6c && sel[2] == 0xa6
          && sel[3] == 0x1f);
}

/* Ответ Router.getAmountsOut: uint[2] = [вход, выход]. */
static const char *rpc_amounts(uint64_t in, uint64_t out)
{
    static char buf[340];
    snprintf(buf, sizeof(buf),
             "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":\"0x"
             "%064x%064x%064llx%064llx\"}",
             0x20, 2, (unsigned long long)in, (unsigned long long)out);
    return buf;
}

/* V2-биржа: котировка через Router.getAmountsOut, разбор массива. */
static void test_v2_quote(void)
{
    tb_error_t err = TB_ERROR_INIT;
    tb_exec_dex_cfg_t cfg;
    mk_cfg(&cfg);
    cfg.venues[0].kind = TB_DEX_V2;
    cfg.venues[0].router = QUOTER; /* роли адреса для теста не важны */

    tb_u256_t q = dec("227435");
    tb_http_replay_step_t steps[] = {
        { .status = 200, .body = rpc_amounts(100000000000000ull, 227435) },
    };
    tb_http_t *http = tb_http_replay_create(steps, 1, &err);
    tb_exec_backend_t b;
    CHECK(tb_exec_dex_init(&b, http, &cfg, &err) == 0);

    tb_route_t r = tb_route_single(&WETH, &USDC, 3000, &err);
    tb_route_set_pool(&r, 0, &POOL_A);
    tb_u256_t out, in0 = dec("100000000000000");
    CHECK(b.quote(&b, &r, &in0, tb_now_ms(), &out, &err) == 0);
    CHECK(tb_u256_cmp(&out, &q) == 0); /* выход = последний элемент */
    b.destroy(&b);
    tb_http_destroy(http);

    /* ответ не массив — ошибка BACKEND с атрибуцией метода */
    tb_http_replay_step_t bad[] = {
        { .status = 200, .body = rpc_ok(dec("123")) },
    };
    http = tb_http_replay_create(bad, 1, &err);
    CHECK(tb_exec_dex_init(&b, http, &cfg, &err) == 0);
    tb_error_t e2 = TB_ERROR_INIT;
    CHECK(b.quote(&b, &r, &in0, tb_now_ms(), &out, &e2) != 0);
    CHECK(e2.code == TB_EXEC_BACKEND);
    CHECK(strstr(e2.message, "getAmountsOut") != NULL);
    b.destroy(&b);
    tb_http_destroy(http);

    /* init fail-fast: V2 без router */
    tb_exec_dex_cfg_t cfg2;
    mk_cfg(&cfg2);
    cfg2.venues[0].kind = TB_DEX_V2;
    memset(&cfg2.venues[0].router, 0, sizeof(tb_addr_t));
    tb_exec_backend_t b2;
    tb_error_t e3 = TB_ERROR_INIT;
    tb_http_replay_step_t none[] = { { .status = 200, .body = "{}" } };
    tb_http_t *h3 = tb_http_replay_create(none, 1, &err);
    CHECK(tb_exec_dex_init(&b2, h3, &cfg2, &e3) != 0);
    CHECK(strstr(e3.message, "router") != NULL);
    tb_http_destroy(h3);

    /* init fail-fast: V3 без quoter */
    tb_exec_dex_cfg_t cfg3;
    mk_cfg(&cfg3);
    memset(&cfg3.venues[0].quoter, 0, sizeof(tb_addr_t));
    tb_exec_backend_t b3;
    tb_error_t e4 = TB_ERROR_INIT;
    tb_http_t *h4 = tb_http_replay_create(none, 1, &err);
    CHECK(tb_exec_dex_init(&b3, h4, &cfg3, &e4) != 0);
    CHECK(strstr(e4.message, "quoter") != NULL);
    tb_http_destroy(h4);
}

/* V2-биржа: проверка пары на цепи через Factory.getPair. */
static void test_verify_pair_v2(void)
{
    tb_error_t err = TB_ERROR_INIT;
    tb_exec_dex_venue_t v;
    memset(&v, 0, sizeof(v));
    strcpy(v.name, "sushi-test");
    strcpy(v.rpc_url, "https://rpc.test/x");
    v.kind = TB_DEX_V2;
    v.router = QUOTER;
    v.factory = A("0xC0AEe478e3658e2610c5F7A4A2E1777cE9e4f2Ac");

    char body[192];
    snprintf(body, sizeof(body),
             "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":\"%s\"}",
             addr_word(&POOL_A));
    tb_http_replay_step_t steps[] = { { .status = 200, .body = body } };
    tb_http_t *http = tb_http_replay_create(steps, 1, &err);
    CHECK(tb_exec_dex_verify_pool(http, &v, &WETH, &USDC, 3000, &POOL_A,
                                  &err) == 0);
    tb_http_destroy(http);

    snprintf(body, sizeof(body),
             "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":\"%s\"}",
             addr_word(&POOL_B));
    tb_http_replay_step_t steps2[] = { { .status = 200, .body = body } };
    http = tb_http_replay_create(steps2, 1, &err);
    CHECK(tb_exec_dex_verify_pool(http, &v, &WETH, &USDC, 3000, &POOL_A,
                                  &err) != 0);
    CHECK(strstr(err.message, "getPair") != NULL);
    CHECK(strstr(err.message, "фабрика") != NULL);
    tb_http_destroy(http);
}

/* Смешанный маршрут: хоп0 — V3 (QuoterV2), хоп1 — V2 (getAmountsOut). */
static void test_mixed_v3_v2_route(void)
{
    tb_error_t err = TB_ERROR_INIT;
    tb_exec_dex_cfg_t cfg;
    mk_cfg(&cfg);

    strcpy(cfg.venues[1].name, "sushi-test");
    strcpy(cfg.venues[1].rpc_url, "https://rpc.test/y");
    cfg.venues[1].kind = TB_DEX_V2;
    cfg.venues[1].router = QUOTER;
    cfg.venues[1].pools[0] = POOL_B;
    cfg.venues[1].pools_n = 1;
    cfg.venues[0].pools_n = 1;
    cfg.venues_n = 2;

    tb_u256_t q1 = dec("2253600"), q2 = dec("1001234567890123");
    tb_http_replay_step_t steps[] = {
        { .status = 200, .body = rpc_ok(q1) }, /* uni V3: WETH -> USDC */
        { .status = 200, .body = rpc_amounts(2253600, 1001234567890123ull) },
    };
    tb_http_t *http = tb_http_replay_create(steps, 2, &err);
    tb_exec_backend_t b;
    CHECK(tb_exec_dex_init(&b, http, &cfg, &err) == 0);

    tb_addr_t toks[3] = { WETH, USDC, WETH };
    uint32_t fees[2] = { 3000, 3000 };
    tb_route_t r = tb_route_encode(toks, fees, 3, &err);
    tb_route_set_pool(&r, 0, &POOL_A);
    tb_route_set_pool(&r, 1, &POOL_B);

    tb_u256_t out, in0 = dec("1000000000000000");
    CHECK(b.quote(&b, &r, &in0, tb_now_ms(), &out, &err) == 0);
    CHECK(tb_u256_cmp(&out, &q2) == 0);

    const tb_exec_dex_stats_t *st = tb_exec_dex_stats(&b);
    CHECK(st->quotes == 1 && st->rpc_calls == 2);

    b.destroy(&b);
    tb_http_destroy(http);
}

static void test_verify_pool(void)
{
    tb_error_t err = TB_ERROR_INIT;
    tb_exec_dex_venue_t v;
    memset(&v, 0, sizeof(v));
    strcpy(v.name, "uni-test");
    strcpy(v.rpc_url, "https://rpc.test/x");
    v.quoter = QUOTER;
    v.factory = A("0x1F98431c8AD9852364AE8ddf06475563B9961697");

    /* 1) фабрика вернула тот же адрес — ок */
    {
        char body[192];
        snprintf(body, sizeof(body),
                 "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":\"%s\"}",
                 addr_word(&POOL_A));
        tb_http_replay_step_t steps[] = { { .status = 200, .body = body } };
        tb_http_t *http = tb_http_replay_create(steps, 1, &err);
        CHECK(http != NULL);
        CHECK(tb_exec_dex_verify_pool(http, &v, &WETH, &USDC, 500, &POOL_A,
                                      &err) == 0);
        tb_http_destroy(http);
    }

    /* 2) фабрика вернула ДРУГОЙ адрес — отказ с атрибуцией */
    {
        char body[192];
        snprintf(body, sizeof(body),
                 "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":\"%s\"}",
                 addr_word(&POOL_B));
        tb_http_replay_step_t steps[] = { { .status = 200, .body = body } };
        tb_http_t *http = tb_http_replay_create(steps, 1, &err);
        CHECK(tb_exec_dex_verify_pool(http, &v, &WETH, &USDC, 500, &POOL_A,
                                      &err) != 0);
        CHECK(err.domain == TB_DOMAIN_USER
              && err.code == TB_EXEC_BACKEND);
        CHECK(strstr(err.message, "фабрика") != NULL);
        tb_http_destroy(http);
    }

    /* 3) пула нет (0x) — отказ: пара/fee не совпадают с цепью */
    {
        tb_http_replay_step_t steps[] = {
            { .status = 200, .body = "{\"result\":\"0x\"}" }
        };
        tb_http_t *http = tb_http_replay_create(steps, 1, &err);
        CHECK(tb_exec_dex_verify_pool(http, &v, &WETH, &USDC, 500, &POOL_A,
                                      &err) != 0);
        CHECK(strstr(err.message, "пула") != NULL);
        tb_http_destroy(http);
    }

    /* 4) фабрика нулевая (venue не V3-типа) — проверка пропускается */
    memset(&v.factory, 0, sizeof(v.factory));
    CHECK(tb_exec_dex_verify_pool(NULL, &v, &WETH, &USDC, 500, &POOL_A,
                                  &err) == 0);
}

static void test_block_number(void)
{
    tb_error_t err = TB_ERROR_INIT;
    tb_exec_dex_cfg_t cfg;
    mk_cfg(&cfg);
    tb_http_replay_step_t steps[] = {
        { .status = 200, .body = "{\"jsonrpc\":\"2.0\",\"id\":1,"
                                 "\"result\":\"0x1234abcd\"}" }
    };
    tb_http_t *http = tb_http_replay_create(steps, 1, &err);
    uint64_t block = 0;
    CHECK(tb_exec_dex_block_number(http, &cfg.venues[0], &block, &err) == 0);
    CHECK(block == 0x1234abcd);
    tb_http_destroy(http);
}

static void test_token_info(void)
{
    tb_error_t err = TB_ERROR_INIT;
    tb_exec_dex_cfg_t cfg;
    mk_cfg(&cfg);

    /* symbol() -> строка ABI "USDC", decimals() -> 6 */
    char sym_body[320];
    snprintf(sym_body, sizeof(sym_body),
             "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":\"%s\"}",
             abi_string("USDC"));
    tb_http_replay_step_t steps[] = {
        { .status = 200, .body = sym_body },
        { .status = 200, .body = "{\"result\":\"0x6\"}" },
    };
    tb_http_t *http = tb_http_replay_create(steps, 2, &err);

    char symbol[16];
    uint8_t decimals = 0;
    CHECK(tb_exec_dex_token_info(http, &cfg.venues[0], &USDC, symbol,
                                 sizeof(symbol), &decimals,
                                 &err) == 0);
    CHECK_STR_EQ(symbol, "USDC");
    CHECK(decimals == 6);
    tb_http_destroy(http);

    /* старые токены: symbol как bytes32 */
    char b32[200];
    snprintf(b32, sizeof(b32),
             "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":\"0x"
             "554e4956320000000000000000000000000000000000000000000000000000"
             "00"
             "\"}");
    tb_http_replay_step_t steps2[] = {
        { .status = 200, .body = b32 },
        { .status = 200, .body = "{\"result\":\"0x12\"}" }, /* 18 */
    };
    http = tb_http_replay_create(steps2, 2, &err);
    CHECK(tb_exec_dex_token_info(http, &cfg.venues[0], &USDC, symbol,
                                 sizeof(symbol), &decimals, &err) == 0);
    CHECK_STR_EQ(symbol, "UNIV2");
    CHECK(decimals == 18);
    tb_http_destroy(http);
}

/* Две биржи: хоп с пулом POOL_A исполняется на venue 0, POOL_B — на
 * venue 1, каждый котируется своим quoter'ом (межбиржевой маршрут). */
static void test_two_venues_route(void)
{
    tb_error_t err = TB_ERROR_INIT;
    tb_exec_dex_cfg_t cfg;
    mk_cfg(&cfg);

    strcpy(cfg.venues[1].name, "cake-test");
    strcpy(cfg.venues[1].rpc_url, "https://rpc.test/y");
    cfg.venues[1].quoter = QUOTER;
    cfg.venues[1].pools[0] = POOL_B;
    cfg.venues[1].pools_n = 1;
    cfg.venues[0].pools_n = 1; /* POOL_B теперь принадлежит только cake */
    cfg.venues_n = 2;

    tb_u256_t q1 = dec("2253600"), q2 = dec("1001234567890123");
    tb_http_replay_step_t steps[] = {
        { .status = 200, .body = rpc_ok(q1) }, /* uni: WETH -> USDC */
        { .status = 200, .body = rpc_ok(q2) }, /* cake: USDC -> WETH */
    };
    tb_http_t *http = tb_http_replay_create(steps, 2, &err);
    tb_exec_backend_t b;
    CHECK(tb_exec_dex_init(&b, http, &cfg, &err) == 0);

    tb_addr_t toks[3] = { WETH, USDC, WETH };
    uint32_t fees[2] = { 3000, 500 };
    tb_route_t r = tb_route_encode(toks, fees, 3, &err);
    tb_route_set_pool(&r, 0, &POOL_A);
    tb_route_set_pool(&r, 1, &POOL_B);

    tb_u256_t out, in0 = dec("1000000000000000");
    CHECK(b.quote(&b, &r, &in0, tb_now_ms(), &out, &err) == 0);
    CHECK(tb_u256_cmp(&out, &q2) == 0);

    const tb_exec_dex_stats_t *st = tb_exec_dex_stats(&b);
    CHECK(st->quotes == 1 && st->rpc_calls == 2);

    b.destroy(&b);
    tb_http_destroy(http);
}

int main(void)
{
    test_quote_multihop();
    test_quote_errors();
    test_submit_poll();
    test_capacity();
    test_selectors_on_site();
    test_v2_quote();
    test_verify_pool();
    test_verify_pair_v2();
    test_block_number();
    test_token_info();
    test_two_venues_route();
    test_mixed_v3_v2_route();
    printf("OK test_backend_dex\n");
    return 0;
}
