/* ==========================================================================
 *  tests/test_mkt_cli.cpp — офлайн-команды менеджера конфигурации market
 *  на временных файлах: dex add/update, asset add/logo (временный cwd),
 *  pool enable/disable, config validate/diff/rollback. Сеть не нужна.
 * ========================================================================== */
#include <initializer_list>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "harness.h"
#include "tb/core/error.h"
#include "tb/user/arb.h"

extern int market_main(int argc, char **argv);

static char g_orig_cwd[4096];

static void chdir_tmp(void)
{
    getcwd(g_orig_cwd, sizeof(g_orig_cwd));
    char t[] = "/tmp/mkt-test-XXXXXX";
    CHECK(mkdtemp(t) != NULL);
    CHECK(chdir(t) == 0);
}

static void write_base_config(const char *path)
{
    FILE *f = fopen(path, "w");
    CHECK(f != NULL);
    fputs("{\n"
          "  \"weth\": \"0xC02aaA39b223FE8D0A0e5C4F27eAD9083C756Cc2\",\n"
          "  \"rpc\": [{\n"
          "    \"name\": \"uniswap-v3\",\n"
          "    \"url\": \"https://ethereum-rpc.publicnode.com\",\n"
          "    \"quoter\": \"0x61fFE014bA17989E743c5F6cB21bF9697530B21e\",\n"
          "    \"factory\": \"0x1F98431c8aD98523631AE4a59f267346ea31F984\"\n"
          "  }],\n"
          "  \"pools\": [{\n"
          "    \"token\": \"0xA0b86991c6218b36c1d19D4a2e9Eb0cE3606eB48\",\n"
          "    \"pool\": \"0x88e6A0c2dDD26FEEb64F039a2c41296FcB3f5640\",\n"
          "    \"fee\": 500,\n"
          "    \"dex\": \"uniswap-v3\"\n"
          "  }],\n"
          "  \"network\": { \"chain_id\": 1 }\n"
          "}\n",
          f);
    fclose(f);
}

static int mkt(std::initializer_list<const char *> args)
{
    char *argv[16] = {0};
    int argc = 0;
    for (const char *a : args)
        argv[argc++] = const_cast<char *>(a);
    return market_main(argc, argv);
}

int main(void)
{
    tb_error_t err = TB_ERROR_INIT;
    chdir_tmp();
    write_base_config("arb_config.json");
    setenv("MARKET_CONFIG", "arb_config.json", 1);
    unsetenv("MARKET_LOGOS"); /* не используется, просто чисто */

    /* ---- 1. dex add + повтор (дубликат отвергнут) ---- */
    CHECK(mkt({"market", "dex", "add", "pancakeswap-v3",
               "--url", "https://ethereum-rpc.publicnode.com",
               "--quoter", "0xB048Bbc1Ee6b733FFfCFb9e9CeF7375518e25997",
               "--factory",
               "0x0BFbCF9fa4f9C56B0F40a671Ad40E0805A091865"})
          == 0);
    CHECK(mkt({"market", "dex", "add", "pancakeswap-v3",
               "--url", "https://x", "--quoter",
               "0xB048Bbc1Ee6b733FFfCFb9e9CeF7375518e25997"})
          == 1); /* уже есть */

    /* ---- 2. dex update: точечная ячейка url ---- */
    CHECK(mkt({"market", "dex", "update", "pancakeswap-v3",
               "--url", "https://other-rpc.example"})
          == 0);

    /* ---- 3. asset add + logo set (лого и символ — разные ячейки) ---- */
    CHECK(mkt({"market", "asset", "add", "USDC",
               "--address",
               "0xA0b86991c6218b36c1d19D4a2e9Eb0cE3606eB48",
               "--chain", "1", "--decimals", "6"})
          == 0);
    CHECK(mkt({"market", "asset", "add", "USDC",
               "--address",
               "0xA0b86991c6218b36c1d19D4a2e9Eb0cE3606eB48"})
          == 1); /* дубликат адреса */
    /* файл-заглушка логотипа */
    FILE *lf = fopen("usdc-logo.svg", "w");
    fputs("<svg/>", lf);
    fclose(lf);
    CHECK(mkt({"market", "asset", "logo", "set", "USDC",
               "--file", "usdc-logo.svg"})
          == 0);
    struct stat st;
    CHECK(stat("logos/token/"
               "0xa0b86991c6218b36c1d19d4a2e9eb0ce3606eb48.svg",
               &st)
          == 0);

    /* ---- 4. pool disable/enable ---- */
    CHECK(mkt({"market", "pool", "disable",
               "0x88e6A0c2dDD26FEEb64F039a2c41296FcB3f5640"})
          == 0);
    {
        tb_arb_cfg_t cfg;
        CHECK(tb_arb_cfg_load(&cfg, "arb_config.json", &err) == 0);
        CHECK(cfg.pools_n == 0); /* выключен — в watchlist не попадает */
    }
    CHECK(mkt({"market", "pool", "enable",
               "0x88e6a0c2ddd26feeb64f039a2c41296fcb3f5640"})
          == 0); /* нижний регистр тоже находит */
    {
        tb_arb_cfg_t cfg;
        CHECK(tb_arb_cfg_load(&cfg, "arb_config.json", &err) == 0);
        CHECK(cfg.pools_n == 1);
        CHECK(cfg.assets_n == 1);
        CHECK(strcmp(cfg.assets[0].symbol, "USDC") == 0);
        CHECK(cfg.assets[0].decimals == 6);
    }

    /* ---- 5. config validate ---- */
    CHECK(mkt({"market", "config", "validate"}) == 0);

    /* ---- 6. diff против снапшота: были изменения ---- */
    CHECK(mkt({"market", "config", "diff"}) == 0); /* просто печатает */

    /* ---- 7. rollback: последняя мутация (enable) отменена ---- */
    CHECK(mkt({"market", "config", "rollback"}) == 0);
    {
        tb_arb_cfg_t cfg;
        CHECK(tb_arb_cfg_load(&cfg, "arb_config.json", &err) == 0);
        CHECK(cfg.dex.venues_n == 2); /* биржа на месте */
        CHECK(cfg.assets_n == 1);     /* актив на месте */
        CHECK(cfg.pools_n == 0);      /* пул снова выключен */
    }

    /* ---- 8. rollback --step 7: в самое начало (снапшотов теперь 7) ---- */
    CHECK(mkt({"market", "config", "rollback", "--step", "7"}) == 0);
    {
        tb_arb_cfg_t cfg;
        CHECK(tb_arb_cfg_load(&cfg, "arb_config.json", &err) == 0);
        CHECK(cfg.dex.venues_n == 1); /* pancakeswap исчез */
        CHECK(cfg.assets_n == 0);     /* USDC исчез */
        CHECK(cfg.pools_n == 1);      /* пул снова включён */
    }

    CHECK(chdir(g_orig_cwd) == 0);
    printf("OK test_mkt_cli\n");
    return 0;
}
