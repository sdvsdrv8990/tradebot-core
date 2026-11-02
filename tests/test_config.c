/* ==========================================================================
 *  tests/test_config.c — загрузка конфигурации арбитража из JSON:
 *  поля, пулы, allowlist, суммы строками, дефолты, ошибки.
 * ========================================================================== */
#include <stdio.h>
#include <string.h>

#include "harness.h"
#include "tb/core/error.h"
#include "tb/user/arb.h"

static const char *GOOD =
    "{"
    "\"weth\": \"0xC02aaA39b223FE8D0A0e5C4F27eAD9083C756Cc2\","
    "\"rpc\": [{ \"name\": \"uniswap-v3\","
    "           \"url\": \"https://ethereum-rpc.publicnode.com\","
    "           \"quoter\": \"0x61fFE014bA17989E743c5F6cB21bF9697530B21e\" },"
    "         { \"name\": \"sushiswap-v2\", \"type\": \"v2\","
    "           \"url\": \"https://ethereum-rpc.publicnode.com\","
    "           \"router\": \"0xd9e1cE17f2641f24aE83637ab66a2cca9C378B9F\","
    "           \"factory\":"
    "             \"0xC0AEe478e3658e2610c5F7A4A2E1777cE9e4f2Ac\" },"
    "         { \"name\": \"uniswap-v3-base\","
    "           \"url\": \"https://base-rpc.publicnode.com\","
    "           \"chain_id\": 8453,"
    "           \"weth\": \"0x4200000000000000000000000000000000000006\","
    "           \"quoter\": \"0x3d4e44Eb1374240CE5F1b871ab261CD16335B76a\","
    "           \"factory\":"
    "             \"0x33128a8fC17869897dcE68Ed026d694621f6FDfD\" }],"
    "\"pools\": ["
    "  { \"token\": \"0xA0b86991c6218b36c1d19D4a2e9Eb0cE3606eB48\","
    "    \"pool\": \"0x88e6A0c2dDD26FEEb64F039a2c41296FcB3f5640\","
    "    \"fee\": 500, \"dex\": \"uniswap-v3\" },"
    "  { \"token\": \"0xA0b86991c6218b36c1d19D4a2e9Eb0cE3606eB48\","
    "    \"pool\": \"0x8ad599c3A0fF1De082011EFDDc58f1908eb6e6D8\","
    "    \"fee\": 3000, \"dex\": \"uniswap-v3\" },"
    "  { \"token\": \"0xA0b86991c6218b36c1d19D4a2e9Eb0cE3606eB48\","
    "    \"pool\": \"0x397fF1542f962076d0BFE58eA045ffa2d347aca0\","
    "    \"fee\": 3000, \"dex\": \"sushiswap-v2\" },"
    "  { \"token\": \"0x833589fCD6eDb6E08f4c7C32D4f71b54bdA02913\","
    "    \"pool\": \"0xd0b53d9277642d899df5c87a3966a349a798f224\","
    "    \"fee\": 500, \"dex\": \"uniswap-v3-base\" }],"
    "\"filters\": { \"freshness_ms\": 15000, \"impact_bp\": 120,"
    "               \"volatility_bp\": 250, \"probe_mult\": 8,"
    "               \"dex_allow\": [\"uniswap-v3\", \"pancakeswap-v3\"] },"
    "\"decision\": { \"poll_ms\": 6000,"
    "                \"probe_wei\": \"100000000000000\","
    "                \"min_trade_wei\": \"200000000000000\","
    "                \"max_trade_wei\": \"8000000000000000\","
    "                \"min_profit_wei\": \"15000000000000\","
    "                \"gas_est_wei\": \"180000000000000\","
    "                \"deadline_ms\": 9000, \"max_pairs\": 2,"
    "                \"fill_delay_min_ms\": 100, \"fill_delay_max_ms\": 200 },"
    "\"web\": { \"port\": 8090, \"bind\": \"0.0.0.0\", \"refresh_ms\": 1500 }"
    "}";

int main(void)
{
    tb_error_t err = TB_ERROR_INIT;
    const char *path = "/tmp/tb_test_config.json";

    /* ---- запись и загрузка ---- */
    FILE *f = fopen(path, "w");
    CHECK(f != NULL);
    fputs(GOOD, f);
    fclose(f);

    tb_arb_cfg_t c;
    CHECK(tb_arb_cfg_load(&c, path, &err) == 0);

    CHECK(!tb_addr_is_native(&c.weth));
    CHECK(c.dex.venues_n == 3);
    CHECK_STR_EQ(c.dex.venues[0].name, "uniswap-v3");
    CHECK_STR_EQ(c.dex.venues[0].rpc_url, "https://ethereum-rpc.publicnode.com");
    CHECK(c.dex.venues[0].kind == TB_DEX_V3);
    CHECK(c.dex.venues[0].quoter.b[0] == 0x61);
    /* В9: реестр — пулы разложены по dex, venues[].pools не в файле */
    CHECK(c.dex.venues[0].pools_n == 2);
    CHECK(c.dex.venues[0].chain_id == 1); /* дефолт network.chain_id */
    CHECK(c.dex.venues[1].kind == TB_DEX_V2);
    CHECK(c.dex.venues[1].router.b[0] == 0xd9);
    CHECK(c.dex.venues[1].pools_n == 1);
    CHECK(c.dex.venues[2].chain_id == 8453);   /* Base из конфига */
    CHECK(c.dex.venues[2].weth.b[19] == 0x06); /* 0x4200…0006 */
    CHECK(c.dex.venues[2].pools_n == 1);

    CHECK(c.pools_n == 4);
    CHECK(c.pools[0].fee == 500);
    CHECK(c.pools[1].fee == 3000);
    CHECK_STR_EQ(c.pools[1].dex, "uniswap-v3");
    CHECK_STR_EQ(c.pools[2].dex, "sushiswap-v2");
    /* мультичейн: пул несёт цепь и хаб своей биржи */
    CHECK(c.pools[2].chain == 1);
    CHECK(tb_addr_eq(&c.pools[2].weth, &c.weth));
    CHECK(c.pools[3].chain == 8453);
    CHECK(c.pools[3].weth.b[19] == 0x06);

    CHECK(c.freshness_max_ms == 15000);
    CHECK(c.liquidity_max_bp == 120);
    CHECK(c.volatility_max_bp == 250);
    CHECK(c.liquidity_probe_mult == 8);
    CHECK(c.dex_allow_n == 2);
    CHECK_STR_EQ(c.dex_allow[1], "pancakeswap-v3");
    CHECK_STR_EQ(c.web.bind, "0.0.0.0"); /* listen-адрес морды */

    CHECK(c.poll_ms == 6000);
    CHECK(c.probe_wei == 100000000000000ull);
    CHECK(c.min_trade_wei == 200000000000000ull);
    CHECK(c.max_trade_wei == 8000000000000000ull);
    CHECK(c.min_profit_wei == 15000000000000ull);
    CHECK(c.gas_est_wei == 180000000000000ull);
    CHECK(c.deadline_ms == 9000);
    CHECK(c.max_pairs_per_poll == 2);
    CHECK(c.dex.fill_delay_min_ms == 100);
    CHECK(c.dex.fill_delay_max_ms == 200);

    /* ---- дефолты ---- */
    tb_arb_cfg_t z;
    memset(&z, 0, sizeof(z));
    tb_arb_defaults(&z);
    CHECK(z.poll_ms == 8000);
    CHECK(z.max_trade_wei == 10000000000000000ull); /* верх контракта */
    CHECK(z.min_trade_wei == 100000000000000ull);
    CHECK(z.min_profit_wei == 10000000000000ull);
    CHECK(z.gas_est_wei == 200000000000000ull);
    CHECK(z.deadline_ms == 8000);
    CHECK(z.max_pairs_per_poll == 1);
    CHECK(z.liquidity_probe_mult == 10);
    CHECK(z.liquidity_max_bp == 100);
    CHECK(z.volatility_max_bp == 300);
    CHECK(z.freshness_max_ms == 24000);

    /* ---- ошибки ---- */
    f = fopen(path, "w");
    fputs("{ не json", f);
    fclose(f);
    tb_error_t e2 = TB_ERROR_INIT;
    CHECK(tb_arb_cfg_load(&c, path, &e2) != 0);
    CHECK(e2.domain == TB_DOMAIN_JSON);

    f = fopen(path, "w");
    fputs("{\"weth\": \"0xZZ\"}", f);
    fclose(f);
    tb_error_t e3 = TB_ERROR_INIT;
    CHECK(tb_arb_cfg_load(&c, path, &e3) != 0);

    tb_error_t e4 = TB_ERROR_INIT;
    CHECK(tb_arb_cfg_load(&c, "/tmp/нет-такого-файла.json", &e4) != 0);
    CHECK(e4.domain == TB_DOMAIN_CORE);

    /* В9: пул ссылается на неизвестную биржу — отказ загрузки */
    f = fopen(path, "w");
    fputs("{\"weth\": \"0xC02aaA39b223FE8D0A0e5C4F27eAD9083C756Cc2\","
          "\"rpc\": [{ \"name\": \"uniswap-v3\","
          "           \"url\": \"https://x\","
          "           \"quoter\":"
          " \"0x61fFE014bA17989E743c5F6cB21bF9697530B21e\" }],"
          "\"pools\": [{ \"token\":"
          " \"0xA0b86991c6218b36c1d19D4a2e9Eb0cE3606eB48\","
          "  \"pool\": \"0x88e6A0c2dDD26FEEb64F039a2c41296FcB3f5640\","
          "  \"fee\": 500, \"dex\": \"нет-такой-биржи\" }]}",
          f);
    fclose(f);
    tb_error_t e5 = TB_ERROR_INIT;
    CHECK(tb_arb_cfg_load(&c, path, &e5) != 0);
    CHECK(strstr(e5.message, "нет в rpc[]") != NULL);

    remove(path);
    printf("OK test_config\n");
    return 0;
}
