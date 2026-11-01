/* ==========================================================================
 *  tests/test_details.c — капсулы деталей: упаковка/разворачивание,
 *  жизненный цикл на ошибке (set чистит, wrap хранит, promote наследует),
 *  продюсеры (dex-хоп на ошибке RPC, кривая decision).
 * ==========================================================================
 */
#define TB_CURRENT_GROUP TB_G_DEMO

#include <stdio.h>
#include <string.h>

#include "harness.h"
#include "tb/core/detail.h"
#include "tb/core/error.h"
#include "tb/exec/backend_dex.h"
#include "tb/net/http.h"
#include "tb/user/arb.h"

static tb_addr_t A(const char *hex)
{
    tb_addr_t a;
    CHECK(tb_addr_from_hex(&a, hex, NULL) == 0);
    return a;
}

int main(void)
{
    /* ---- упаковка/разворачивание: net raw ---- */
    {
        uint8_t pack[TB_DETAIL_DATA_MAX];
        size_t n = tb_detail_pack_net_raw(pack, sizeof(pack), 429,
                                          "{\"error\":\"lim\"}", 16);
        tb_detail_t d = { .group = TB_G_NET, .cmd = TB_CMD_NET_RAW,
                          .data_len = (uint8_t)n };
        memcpy(d.data, pack, n);
        char buf[192];
        CHECK(tb_detail_expand(&d, buf, sizeof(buf)) > 0);
        CHECK(strstr(buf, "HTTP 429") != NULL);
        CHECK(strstr(buf, "{\"error\":\"lim\"}") != NULL);
    }

    /* ---- упаковка/разворачивание: dex hop ---- */
    {
        uint8_t pack[TB_DETAIL_DATA_MAX];
        tb_addr_t quoter = A("0x61fFE014bA17989E743c5F6cB21bF9697530B21e");
        size_t n = tb_detail_pack_dex_hop(pack, sizeof(pack), quoter.b,
                                          "0xc6a5026a0011...", "0x");
        tb_detail_t d = { .group = TB_G_DEX, .cmd = TB_CMD_DEX_HOP,
                          .data_len = (uint8_t)n };
        memcpy(d.data, pack, n);
        char buf[192];
        CHECK(tb_detail_expand(&d, buf, sizeof(buf)) > 0);
        CHECK(strstr(buf, "to=0x61ffe014") != NULL);
        CHECK(strstr(buf, "calldata=0xc6a5026a") != NULL);
        CHECK(strstr(buf, "result=0x") != NULL);
    }

    /* ---- упаковка/разворачивание: кривая decision ---- */
    {
        uint8_t pack[TB_DETAIL_DATA_MAX];
        uint64_t sz[3] = { 1000, 8000, 16000 }, out[3] = { 990, 7990, 16100 };
        size_t n = tb_detail_pack_curve(pack, sizeof(pack), sz, out);
        tb_detail_t d = { .group = TB_G_DECISION, .cmd = TB_CMD_DECIDE_CURVE,
                          .data_len = (uint8_t)n };
        memcpy(d.data, pack, n);
        char buf[192];
        CHECK(tb_detail_expand(&d, buf, sizeof(buf)) > 0);
        CHECK(strstr(buf, "1000→990") != NULL);
        CHECK(strstr(buf, "16000→16100") != NULL);
    }

    /* ---- жизненный цикл на ошибке ---- */
    {
        tb_error_t err = TB_ERROR_INIT;
        uint8_t pack[TB_DETAIL_DATA_MAX];
        size_t n = tb_detail_pack_net_raw(pack, sizeof(pack), 503, "oops", 4);
        tb_error_detail(&err, TB_G_NET, TB_CMD_NET_RAW, pack, n);

        TB_ERROR_SET(&err, TB_DOMAIN_NET, TB_NET_RESPONSE, "транспорт");
        CHECK(err.details_n == 0); /* set очищает капсулы */

        tb_error_detail(&err, TB_G_NET, TB_CMD_NET_RAW, pack, n);
        TB_ERROR_WRAP(&err, "конвейер");
        CHECK(err.details_n == 1); /* wrap сохраняет */

        TB_ERROR_PROMOTE(&err, TB_DOMAIN_USER, TB_EXEC_BACKEND, "выше");
        CHECK(err.details_n == 1); /* promote наследует происхождение */
        CHECK(err.group == TB_G_DEMO);

        char buf[256];
        CHECK(tb_error_format_details(&err, buf, sizeof(buf)) > 0);
        CHECK(strstr(buf, "[net/1]") != NULL);
        CHECK(strstr(buf, "HTTP 503") != NULL);

        /* кольцо: третья капсула замещает старейшую (net/raw в слоте 0) */
        tb_error_detail(&err, TB_G_DEX, TB_CMD_DEX_HOP, pack, 52);
        tb_error_detail(&err, TB_G_DECISION, TB_CMD_DECIDE_CURVE, pack, 48);
        CHECK(err.details[0].cmd == TB_CMD_DECIDE_CURVE); /* вытеснила net */
        CHECK(err.details[1].cmd == TB_CMD_DEX_HOP);
    }

    /* ---- продюсер: dex-хоп кладёт капсулу на ошибку RPC ---- */
    {
        tb_error_t err = TB_ERROR_INIT;
        tb_exec_dex_cfg_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        strcpy(cfg.venues[0].name, "uni-test");
        strcpy(cfg.venues[0].rpc_url, "https://rpc.test/x");
        cfg.venues[0].quoter = A("0x61fFE014bA17989E743c5F6cB21bF9697530B21e");
        cfg.venues[0].pools[0] = A("0x1111111111111111111111111111111111111111");
        cfg.venues[0].pools_n = 1;
        cfg.venues_n = 1;

        /* пустой result \"0x\" — слепая зона отладки Quoter */
        tb_http_replay_step_t steps[] = {
            { .status = 200, .body = "{\"result\":\"0x\"}" },
        };
        tb_http_t *http = tb_http_replay_create(steps, 1, &err);
        CHECK(http != NULL);
        tb_exec_backend_t b;
        CHECK(tb_exec_dex_init(&b, http, &cfg, &err) == 0);

        tb_addr_t weth = A("0xC02aaA39b223FE8D0A0e5C4F27eAD9083C756Cc2");
        tb_addr_t usdc = A("0xA0b86991c6218b36c1d19D4a2e9Eb0cE3606eB48");
        tb_route_t r = tb_route_single(&weth, &usdc, 500, &err);
        tb_route_set_pool(&r, 0, &cfg.venues[0].pools[0]);
        tb_u256_t in, out;
        tb_u256_from_dec(&in, "1000000000000000", 16, NULL);
        CHECK(b.quote(&b, &r, &in, 0, &out, &err) != 0);
        CHECK(err.details_n >= 1);
        CHECK(err.details[0].group == TB_G_DEX);
        CHECK(err.details[0].cmd == TB_CMD_DEX_HOP);
        char buf[256];
        tb_error_format_details(&err, buf, sizeof(buf));
        CHECK(strstr(buf, "to=0x61ffe014") != NULL);
        CHECK(strstr(err.message, "rlen=2") != NULL);

        b.destroy(&b);
        tb_http_destroy(http);
    }

    printf("OK test_details\n");
    return 0;
}
