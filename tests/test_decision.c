/* ==========================================================================
 *  tests/test_decision.c — Decision Engine на синтетических кривых
 *  котировок: минимальная выгодная ставка, верх контракта, отсутствие
 *  прибыли, порог размера, min_out, ошибки котировщика.
 * ========================================================================== */
#include <stdio.h>
#include <string.h>

#include "harness.h"
#include "tb/core/error.h"
#include "tb/exec/route.h"
#include "tb/user/arb.h"

static tb_addr_t TOK = { { 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
                           0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
                           0xaa, 0xaa, 0xaa, 0xaa } };

static tb_addr_t POOL_A = { { 1 } };
static tb_addr_t POOL_B = { { 2 } };

/* Сценарий котировщика: направления задаются пулом.
 *   buy (POOL_A): tokens = size * num / den
 *   sell (POOL_B): wei = tokens * (x% ) * snum / (sden * 100)
 * Порог TH: ниже порога x = x_below процентов, выше — x_above. */
typedef struct curve {
    uint64_t num, den, snum, sden;
    uint64_t threshold; /* 0 — без порога */
    uint64_t x_below, x_above;
    int      fail_after; /* после N вызовов — ошибка; 0 = без ошибок */
    int      calls;
} curve_t;

static int synth_quote(void *ctx, const tb_addr_t *pool, uint32_t fee,
                       const tb_addr_t *tin, const tb_addr_t *tout,
                       const tb_u256_t *amount, tb_u256_t *out,
                       tb_error_t *err)
{
    (void)fee; (void)tin; (void)tout;
    curve_t *c = ctx;
    if (c->fail_after && ++c->calls > c->fail_after) {
        TB_ERROR_SET(err, TB_DOMAIN_USER, TB_EXEC_BACKEND,
                     "синтетика: отказ");
        return -1;
    }

    uint64_t amt = amount->w[0];
    if (tb_addr_eq(pool, &POOL_A)) {
        out->w[0] = amt * c->num / c->den; /* покупка: получаем токены */
        out->w[1] = out->w[2] = out->w[3] = 0;
        return 0;
    }
    /* продажа: множитель — проценты (50 = половина) */
    uint64_t mult = amt >= c->threshold ? c->x_above : c->x_below;
    out->w[0] = amt * mult * c->snum / (c->sden * 100);
    out->w[1] = out->w[2] = out->w[3] = 0;
    return 0;
}

static tb_arb_cfg_t mk_cfg(void)
{
    tb_arb_cfg_t c;
    memset(&c, 0, sizeof(c));
    c.min_trade_wei = 1000;
    c.max_trade_wei = 128000; /* верх контракта */
    c.min_profit_wei = 5;
    c.gas_est_wei = 10;
    return c;
}

static tb_arb_pair_in_t mk_pair(void)
{
    tb_arb_snap_t buy_snap, sell_snap;
    memset(&buy_snap, 0, sizeof(buy_snap));
    memset(&sell_snap, 0, sizeof(sell_snap));
    buy_snap.pool = POOL_A;
    buy_snap.token = TOK;
    buy_snap.fee = 500;
    sell_snap.pool = POOL_B;
    sell_snap.token = TOK;
    sell_snap.fee = 3000;

    static tb_arb_snap_t bs, ss; /* пара ссылается на живые снапшоты */
    bs = buy_snap;
    ss = sell_snap;
    tb_arb_pair_in_t p = { .buy = &bs, .sell = &ss };
    return p;
}

int main(void)
{
    tb_error_t err = TB_ERROR_INIT;
    tb_arb_pair_in_t p = mk_pair();
    tb_arb_pair_res_t r;

    /* ---- 1. Выгодно на любом размере: ставка = минималка ---- */
    {
        tb_arb_cfg_t c = mk_cfg();
        curve_t cv = { .num = 11, .den = 10, .snum = 12, .sden = 11,
                       .x_below = 100, .x_above = 100 };
        /* цикл = size * (11/10) * (12/11) = size * 1.2 */
        CHECK(tb_arb_eval_pair(&c, &p, synth_quote, &cv, &r, &err) == 1);
        CHECK(r.ok);
        CHECK(r.size.w[0] == 1000); /* минималка */
        CHECK(r.sell_out.w[0] == 1000 * 11 / 10 * 12 / 11);
        /* min_out = size + min_profit + gas = 1000 + 5 + 10 */
        CHECK(r.min_out.w[0] == 1015);
        CHECK(r.probes >= 2);
    }

    /* ---- 2. Никогда не выгодно: отказ «на верхе контракта» ---- */
    {
        tb_arb_cfg_t c = mk_cfg();
        curve_t cv = { .num = 10, .den = 10, .snum = 98, .sden = 100,
                       .x_below = 100, .x_above = 100 };
        int rc = tb_arb_eval_pair(&c, &p, synth_quote, &cv, &r, &err);
        CHECK(rc == 0);
        CHECK(!r.ok);
        CHECK(strstr(r.reason, "верхе контракта") != NULL);
        CHECK(r.probes == 1); /* сэкономили вызовы */
    }

    /* ---- 3. Выгодно только с порога TH: поиск минималки удвоением ---- */
    {
        tb_arb_cfg_t c = mk_cfg();
        /* ниже 10000: продажа в убыток (x50%), выше — в плюс (x105%) */
        curve_t cv = { .num = 10, .den = 10, .snum = 100, .sden = 100,
                       .threshold = 10000, .x_below = 50, .x_above = 105 };
        CHECK(tb_arb_eval_pair(&c, &p, synth_quote, &cv, &r, &err) == 1);
        CHECK(r.ok);
        CHECK(r.size.w[0] >= 10000); /* не ниже порога */
        CHECK(r.size.w[0] < 20000);  /* найдено удвоением/уточнением */
        CHECK(r.sell_out.w[0] > r.size.w[0] + 15); /* прибыль реальна */
    }

    /* ---- 4. Ошибка котировщика: rc -1, err заполнен ---- */
    {
        tb_arb_cfg_t c = mk_cfg();
        curve_t cv = { .num = 11, .den = 10, .snum = 12, .sden = 11,
                       .x_below = 100, .x_above = 100, .fail_after = 1 };
        tb_error_t qerr = TB_ERROR_INIT;
        CHECK(tb_arb_eval_pair(&c, &p, synth_quote, &cv, &r, &qerr) == -1);
        CHECK(qerr.domain == TB_DOMAIN_USER);
        CHECK(qerr.code == TB_EXEC_BACKEND);
    }

    printf("OK test_decision\n");
    return 0;
}
