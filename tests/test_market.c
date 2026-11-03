/* ==========================================================================
 *  tests/test_market.c — приёмка P0/P1 из плана ревью:
 *   - В1: устаревший снапшот не участвует в сравнении курсов (пара
 *     исключена, пул в таблице с fresh:false);
 *   - В6: пара с разными блоками не сравнивается;
 *   - два пула одной пары — разные адреса/комиссии в снапшоте;
 *   - В7: реальная котировка лучшей пары рядом с оценкой;
 *   - В2: валидатор конфига (двойное объявление рассинхронизировано
 *     быть не может).
 * ========================================================================== */
#include <stdio.h>
#include <string.h>

#include "harness.h"
#include "tb/core/error.h"
#include "tb/core/time.h"
#include "tb/user/arb.h"

#define WETH_HEX "0xC02aaA39b223FE8D0A0e5C4F27eAD9083C756Cc2"
#define USDC_HEX "0xA0b86991c6218b36c1d19D4a2e9Eb0cE3606eB48"
#define USDT_HEX "0xdAC17F958D2ee523a2206206994597C13D831ec7"

static tb_addr_t A(const char *hex)
{
    tb_addr_t a;
    CHECK(tb_addr_from_hex(&a, hex, NULL) == 0);
    return a;
}

static tb_arb_snap_t mk_snap(uint32_t idx, const char *pool_hex,
                             const char *tok_hex, uint32_t fee,
                             const char *dex, uint64_t block, uint64_t ts,
                             uint64_t buy, const char *symbol)
{
    tb_arb_snap_t s;
    memset(&s, 0, sizeof(s));
    s.pool_idx = idx;
    s.pool = A(pool_hex);
    s.token = A(tok_hex);
    s.fee = fee;
    snprintf(s.dex, sizeof(s.dex), "%s", dex);
    snprintf(s.symbol, sizeof(s.symbol), "%s", symbol);
    s.decimals = 6;
    s.block = block;
    s.ts_ms = ts;
    s.buy_out = tb_u256_from_u64(buy);
    s.big_out = tb_u256_from_u64(buy * 9); /* probe*10 минус лёгкое влияние */
    s.sell_back = tb_u256_from_u64(99000); /* круг ≈ −1%: живой пул */
    s.complete = true;
    return s;
}

static void mk_cfg(tb_arb_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->weth = A(WETH_HEX);
    cfg->freshness_max_ms = 15000;
    cfg->poll_ms = 5000;
    cfg->probe_wei = 100000000000000ull; /* 1e14 */
}

/* разные пулы Uniswap: USDC/WETH 0.05% и 0.3% (нижний регистр — как в
 * JSON снапшота; конфиг может быть в любом регистре) */
#define POOL_UNI_USDC_500 "0x88e6a0c2ddd26feeb64f039a2c41296fcb3f5640"
#define POOL_UNI_USDC_3000 "0x8ad599c3a0ff1de082011efddc58f1908eb6e6d8"
#define POOL_UNI_USDT_500 "0x11b815efb8f581194ae79006d24e0d814b7697f6"
#define POOL_PAN_USDC_500 "0x1ac1a8feaaea1900c4166deeed0c11cc10669d36"

static void test_pair_fresh_same_block(void)
{
    tb_arb_cfg_t cfg;
    mk_cfg(&cfg);
    uint64_t now = tb_now_ms();

    tb_arb_snap_t snaps[2];
    /* покупка на fee-500 «дешевле» (больше токенов за пробу) */
    snaps[0] = mk_snap(0, POOL_UNI_USDC_500, USDC_HEX, 500, "uniswap-v3",
                       12345, now - 100, 20000, "USDC");
    snaps[1] = mk_snap(1, POOL_UNI_USDC_3000, USDC_HEX, 3000, "uniswap-v3",
                       12345, now - 100, 19900, "USDC");

    char json[4096];
    CHECK(tb_arb_market_json(&cfg, snaps, 2, now, NULL, json,
                             sizeof(json)) == 0);
    CHECK(strstr(json, "\"spread_bp\":50") != NULL); /* 20000/19900 */
    CHECK(strstr(json, "\"symbol\":\"USDC\"") != NULL);
    CHECK(strstr(json, "\"profit_est\":") != NULL);
    /* два пула — разные адреса и комиссии в снапшоте */
    CHECK(strstr(json, POOL_UNI_USDC_500) != NULL);
    CHECK(strstr(json, POOL_UNI_USDC_3000) != NULL);
    CHECK(strstr(json, "\"fee\":\"0.0500%\"") != NULL);
    CHECK(strstr(json, "\"fee\":\"0.3000%\"") != NULL);
    CHECK(strstr(json, "\"block\":12345") != NULL);
}

static void test_stale_snap_excluded(void)
{
    tb_arb_cfg_t cfg;
    mk_cfg(&cfg);
    uint64_t now = tb_now_ms();

    tb_arb_snap_t snaps[2];
    snaps[0] = mk_snap(0, POOL_UNI_USDC_500, USDC_HEX, 500, "uniswap-v3",
                       12345, now - 100, 20000, "USDC");
    /* второй снапшот устарел: за fmax */
    snaps[1] = mk_snap(1, POOL_UNI_USDC_3000, USDC_HEX, 3000, "uniswap-v3",
                       12345, now - cfg.freshness_max_ms - 1, 19900, "USDC");

    char json[4096];
    CHECK(tb_arb_market_json(&cfg, snaps, 2, now, NULL, json,
                             sizeof(json)) == 0);
    CHECK(strstr(json, "\"spread_bp\"") == NULL); /* В1: пары нет */
    /* но пул виден в таблице с явной пометкой */
    CHECK(strstr(json, POOL_UNI_USDC_3000) != NULL);
    CHECK(strstr(json, "\"fresh\":false") != NULL);
    CHECK(strstr(json, "\"fresh\":true") != NULL);
}

static void test_different_chains_excluded(void)
{
    tb_arb_cfg_t cfg;
    mk_cfg(&cfg);
    uint64_t now = tb_now_ms();

    tb_arb_snap_t snaps[2];
    snaps[0] = mk_snap(0, POOL_UNI_USDC_500, USDC_HEX, 500, "uniswap-v3",
                       12345, now - 100, 20000, "USDC");
    /* та же пара, но пул другой ЦЕПИ (base): спред «между цепями» —
     * бессмысленный, курс другой вселенной */
    snaps[1] = mk_snap(1, POOL_UNI_USDC_3000, USDC_HEX, 3000, "uniswap-v3",
                       12345, now - 100, 19900, "USDC");
    snaps[0].chain = 1;
    snaps[1].chain = 8453;

    char json[4096];
    CHECK(tb_arb_market_json(&cfg, snaps, 2, now, NULL, json,
                             sizeof(json)) == 0);
    CHECK(strstr(json, "\"spread_bp\"") == NULL); /* разные цепи */

    /* одна цепь — пара возвращается */
    snaps[1].chain = 1;
    CHECK(tb_arb_market_json(&cfg, snaps, 2, now, NULL, json,
                             sizeof(json)) == 0);
    CHECK(strstr(json, "\"spread_bp\":50") != NULL);
    CHECK(strstr(json, "\"chain\":\"eth\"") != NULL);
}

static void test_different_blocks_excluded(void)
{
    tb_arb_cfg_t cfg;
    mk_cfg(&cfg);
    uint64_t now = tb_now_ms();

    tb_arb_snap_t snaps[2];
    snaps[0] = mk_snap(0, POOL_UNI_USDC_500, USDC_HEX, 500, "uniswap-v3",
                       12345, now - 100, 20000, "USDC");
    snaps[1] = mk_snap(1, POOL_UNI_USDC_3000, USDC_HEX, 3000, "uniswap-v3",
                       12346 /* другой блок */, now - 100, 19900, "USDC");

    char json[4096];
    CHECK(tb_arb_market_json(&cfg, snaps, 2, now, NULL, json,
                             sizeof(json)) == 0);
    CHECK(strstr(json, "\"spread_bp\"") == NULL); /* В6: не сравниваем */

    /* блоки неизвестны (0) — latest каждого пула разные состояния
     * цепи: сравнение ЗАПРЕЩЕНО (P2; прежде 0==0 молча сравнивалось —
     * противоречило собственному комментарию кода) */
    snaps[0].block = 0;
    snaps[1].block = 0;
    CHECK(tb_arb_market_json(&cfg, snaps, 2, now, NULL, json,
                             sizeof(json)) == 0);
    CHECK(strstr(json, "\"spread_bp\"") == NULL);
}

/* Публикация батча: свежесть пар/пулов от pub_ms, не от ts_ms сбора.
 * Батч при медленном RPC живёт дольше окна — к публикации данные были
 * бы уже «устаревшими», пары гасли волнами. */
static void test_pub_freshness(void)
{
    tb_arb_cfg_t cfg;
    mk_cfg(&cfg);
    /* время — СИНТЕТИЧЕСКОЕ: tb_now_ms() монотонно от ЗАГРУЗКИ, у
     * свежего CI-раннера аптайм < 120 c и «now - 120000» андерафлоу
     * (возраст = 0, «устаревшая» пара жива). Сдвиг гарантирует запас */
    uint64_t now = tb_now_ms() + 1000000000ull;

    /* сбор давно, публикация только что: пара ЖИВА */
    tb_arb_snap_t snaps[2];
    snaps[0] = mk_snap(0, POOL_UNI_USDC_500, USDC_HEX, 500, "uniswap-v3",
                       12345, now - 120000, 20000, "USDC");
    snaps[1] = mk_snap(1, POOL_UNI_USDC_3000, USDC_HEX, 3000, "uniswap-v3",
                       12345, now - 120000, 19900, "USDC");
    snaps[0].pub_ms = snaps[1].pub_ms = now - 1000;

    char json[4096];
    CHECK(tb_arb_market_json(&cfg, snaps, 2, now, NULL, json,
                             sizeof(json)) == 0);
    CHECK(strstr(json, "\"spread_bp\":50") != NULL);
    CHECK(strstr(json, "\"fresh\":true") != NULL);

    /* публикация сама устарела — пара гаснет */
    snaps[0].pub_ms = snaps[1].pub_ms = now - cfg.freshness_max_ms - 1;
    CHECK(tb_arb_market_json(&cfg, snaps, 2, now, NULL, json,
                             sizeof(json)) == 0);
    CHECK(strstr(json, "\"spread_bp\"") == NULL);
    CHECK(strstr(json, "\"fresh\":false") != NULL);

    /* без pub_ms (ручная сборка) — прежняя семантика от ts_ms */
    snaps[0].pub_ms = snaps[1].pub_ms = 0;
    tb_arb_snap_t fresh2[2];
    fresh2[0] = snaps[0];
    fresh2[1] = snaps[1];
    fresh2[0].ts_ms = fresh2[1].ts_ms = now - 100;
    CHECK(tb_arb_market_json(&cfg, fresh2, 2, now, NULL, json,
                             sizeof(json)) == 0);
    CHECK(strstr(json, "\"spread_bp\":50") != NULL);
}

static void test_route_quote_section(void)
{
    tb_arb_cfg_t cfg;
    mk_cfg(&cfg);
    uint64_t now = tb_now_ms();

    tb_arb_route_quote_t rq;
    memset(&rq, 0, sizeof(rq));
    rq.ok = true;
    snprintf(rq.symbol, sizeof(rq.symbol), "USDC");
    snprintf(rq.buy_dex, sizeof(rq.buy_dex), "uniswap-v3");
    snprintf(rq.sell_dex, sizeof(rq.sell_dex), "pancakeswap-v3");
    rq.spread_bp = 50;
    rq.in_wei = tb_u256_from_u64(100000000000000ull);
    rq.out_wei = tb_u256_from_u64(99000000000000ull);
    rq.profit_neg = true; /* реальность больнее оценки */
    rq.profit_wei = tb_u256_from_u64(1000000000000ull);

    char json[4096];
    CHECK(tb_arb_market_json(&cfg, NULL, 0, now, &rq, json,
                             sizeof(json)) == 0);
    CHECK(strstr(json, "\"quote\":{") != NULL);
    CHECK(strstr(json, "\"profit_wei\":\"-1000000000000\"") != NULL);
    CHECK(strstr(json, "\"sell\":\"pancakeswap-v3\"") != NULL);
}

static void mk_valid_cfg(tb_arb_cfg_t *cfg)
{
    mk_cfg(cfg);
    strcpy(cfg->dex.venues[0].name, "uniswap-v3");
    cfg->dex.venues[0].pools[0] = A(POOL_UNI_USDC_500);
    cfg->dex.venues[0].pools[1] = A(POOL_UNI_USDC_3000);
    cfg->dex.venues[0].pools_n = 2;
    cfg->dex.venues_n = 1;

    cfg->pools[0] = (tb_arb_pool_t){
        .token = A(USDC_HEX), .pool = A(POOL_UNI_USDC_500),
        .fee = 500, .dex = "uniswap-v3",
    };
    cfg->pools[1] = (tb_arb_pool_t){
        .token = A(USDC_HEX), .pool = A(POOL_UNI_USDC_3000),
        .fee = 3000, .dex = "uniswap-v3",
    };
    cfg->pools_n = 2;
}

static void test_validator(void)
{
    tb_error_t err = TB_ERROR_INIT;
    tb_arb_cfg_t cfg;

    mk_valid_cfg(&cfg);
    CHECK(tb_arb_cfg_validate(&cfg, &err) == 0);

    /* В9: имя биржи не объявлено в rpc[] */
    strcpy(cfg.pools[1].dex, "sushiswap-v2");
    CHECK(tb_arb_cfg_validate(&cfg, &err) != 0);
    CHECK(strstr(err.message, "нет в rpc[]") != NULL);

    /* В9: пул не приписан своей бирже (защита ручной сборки cfg) */
    mk_valid_cfg(&cfg);
    cfg.pools[1].pool = A(POOL_UNI_USDT_500);
    CHECK(tb_arb_cfg_validate(&cfg, &err) != 0);
    CHECK(strstr(err.message, "не приписан") != NULL);

    /* В9: один адрес у двух бирж */
    mk_valid_cfg(&cfg);
    strcpy(cfg.dex.venues[1].name, "pancakeswap-v3");
    cfg.dex.venues[1].pools[0] = A(POOL_UNI_USDC_500);
    cfg.dex.venues[1].pools_n = 1;
    cfg.dex.venues_n = 2;
    CHECK(tb_arb_cfg_validate(&cfg, &err) != 0);
    CHECK(strstr(err.message, "двум биржам") != NULL);

    /* В2: дубликат адреса в pools[] */
    mk_valid_cfg(&cfg);
    cfg.pools[1].pool = cfg.pools[0].pool;
    CHECK(tb_arb_cfg_validate(&cfg, &err) != 0);
    CHECK(strstr(err.message, "дважды") != NULL);

    /* fee обязан быть ненулевым */
    mk_valid_cfg(&cfg);
    cfg.pools[1].fee = 0;
    CHECK(tb_arb_cfg_validate(&cfg, &err) != 0);
    CHECK(strstr(err.message, "fee нулевой") != NULL);

    /* V2: комиссия пары фиксирована 3000 — иное значение ошибка */
    mk_valid_cfg(&cfg);
    cfg.dex.venues[0].kind = TB_DEX_V2;
    CHECK(tb_arb_cfg_validate(&cfg, &err) != 0);
    CHECK(strstr(err.message, "V2") != NULL);
    CHECK(strstr(err.message, "3000") != NULL);
    /* те же пулы с fee 3000 — валидны */
    cfg.pools[0].fee = 3000;
    cfg.pools[1].fee = 3000;
    CHECK(tb_arb_cfg_validate(&cfg, &err) == 0);

    /* пустой watchlist */
    mk_valid_cfg(&cfg);
    cfg.pools_n = 0;
    CHECK(tb_arb_cfg_validate(&cfg, &err) != 0);
}

static void test_block0_excluded(void)
{
    /* блок неизвестен (0) — latest каждого пула разные состояния цепи,
     * пара не собирается (P2: прежний код сравнивал 0==0) */
    tb_arb_cfg_t cfg;
    mk_cfg(&cfg);
    uint64_t now = tb_now_ms();
    tb_arb_snap_t snaps[2];
    snaps[0] = mk_snap(0, POOL_UNI_USDC_500, USDC_HEX, 500, "uniswap-v3",
                       0, now - 100, 20000, "USDC");
    snaps[1] = mk_snap(1, POOL_PAN_USDC_500, USDC_HEX, 500,
                       "pancakeswap-v3", 0, now - 100, 21000, "USDC");
    char json[4096];
    CHECK(tb_arb_market_json(&cfg, snaps, 2, now, NULL, json,
                             sizeof(json)) == 0);
    CHECK(strstr(json, "\"spread_bp\"") == NULL); /* пар нет */
}

static void test_same_dex_marked(void)
{
    /* пара одного DEX (кросс-тир) помечена same_dex — UI отличит её
     * от межбиржевой */
    tb_arb_cfg_t cfg;
    mk_cfg(&cfg);
    uint64_t now = tb_now_ms();
    tb_arb_snap_t snaps[2];
    snaps[0] = mk_snap(0, POOL_UNI_USDC_500, USDC_HEX, 500, "uniswap-v3",
                       12345, now - 100, 20000, "USDC");
    snaps[1] = mk_snap(1, POOL_UNI_USDC_3000, USDC_HEX, 3000, "uniswap-v3",
                       12345, now - 100, 19900, "USDC");
    char json[4096];
    CHECK(tb_arb_market_json(&cfg, snaps, 2, now, NULL, json,
                             sizeof(json)) == 0);
    CHECK(strstr(json, "\"spread_bp\":50") != NULL); /* пара собрана */
    CHECK(strstr(json, "\"same_dex\":true") != NULL);

    /* межбиржевая — false */
    snaps[1] = mk_snap(1, POOL_PAN_USDC_500, USDC_HEX, 500,
                       "pancakeswap-v3", 12345, now - 100, 19900, "USDC");
    CHECK(tb_arb_market_json(&cfg, snaps, 2, now, NULL, json,
                             sizeof(json)) == 0);
    CHECK(strstr(json, "\"same_dex\":false") != NULL);
}

int main(void)
{
    test_pair_fresh_same_block();
    test_stale_snap_excluded();
    test_different_chains_excluded();
    test_different_blocks_excluded();
    test_block0_excluded();
    test_same_dex_marked();
    test_pub_freshness();
    test_route_quote_section();
    test_validator();
    printf("OK test_market\n");
    return 0;
}
