/* ==========================================================================
 *  tests/test_decision.c — Decision Engine на синтетических кривых
 *  котировок: точка безубыточности, оптимальный объём (пик кривой),
 *  интерьерные окна (прибыль только внутри диапазона), живой газ пары,
 *  min_out, ошибки котировщика.
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
 *   sell (POOL_B): wei = tokens * (x%) * snum / (sden * 100)
 * Порог TH: ниже порога x = x_below, выше — x_above.
 * Затухание (decay): x = x_above − amt×decay_num/decay_den — вогнутая
 * кривая с внутренним пиком прибыли. */
typedef struct curve {
    uint64_t num, den, snum, sden;
    uint64_t threshold; /* 0 — без порога */
    uint64_t x_below, x_above;
    uint64_t decay_num, decay_den; /* 0 — без затухания */
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
    if (c->decay_den)
        mult = c->x_above > amt * c->decay_num / c->decay_den
                   ? c->x_above - amt * c->decay_num / c->decay_den
                   : 0;
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

/* требуемая прибыль: min_profit + size×TB_ARB_RISK_BP/10000 */
static uint64_t required(uint64_t size)
{
    return 5 + size * TB_ARB_RISK_BP / 10000;
}

int main(void)
{
    tb_error_t err = TB_ERROR_INIT;
    tb_arb_pair_in_t p = mk_pair();
    tb_arb_pair_res_t r;

    /* ---- 1. Линейный рост прибыли: оптимум = верх контракта ---- */
    {
        tb_arb_cfg_t c = mk_cfg();
        curve_t cv = { .num = 11, .den = 10, .snum = 12, .sden = 11,
                       .x_below = 100, .x_above = 100 };
        /* цикл = size * 1.2; прибыль растёт до самого верха */
        CHECK(tb_arb_eval_pair(&c, &p, synth_quote, &cv, &r, &err) == 1);
        CHECK(r.ok);
        CHECK(r.size.w[0] == 1000); /* безубыточность на полу */
        CHECK(r.sell_out.w[0] == 1000 * 11 / 10 * 12 / 11);
        CHECK(r.opt_size.w[0] == 128000); /* пик на верхе контракта */
        CHECK(r.opt_sell_out.w[0] == 128000 * 12 / 10);
        CHECK(r.opt_profit.w[0]
              == 128000 * 12 / 10 - 128000 - 10 - required(128000));
        /* min_out = opt + газ + требуемая = ровно точка нулевой прибыли */
        CHECK(r.min_out.w[0] == 128000 + 10 + required(128000));
        CHECK(r.probes >= 3);
        CHECK(r.gas_cost_wei == 10); /* фолбэк из конфига */
    }

    /* ---- 2. Никогда не выгодно: отказ «нет прибыли в диапазоне» ---- */
    {
        tb_arb_cfg_t c = mk_cfg();
        curve_t cv = { .num = 10, .den = 10, .snum = 98, .sden = 100,
                       .x_below = 100, .x_above = 100 };
        int rc = tb_arb_eval_pair(&c, &p, synth_quote, &cv, &r, &err);
        CHECK(rc == 0);
        CHECK(!r.ok);
        CHECK(strstr(r.reason, "нет прибыли") != NULL);
        CHECK(r.probes >= 2); /* верх + пол + лестница — все в минусе */
    }

    /* ---- 3. Выгодно с порога TH: безубыточность удвоением ---- */
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
        /* выше порога маржа постоянна: оптимум на верхе */
        CHECK(r.opt_size.w[0] == 128000);
        CHECK(r.opt_profit.w[0]
              == 128000 * 105 / 100 - 128000 - 10 - required(128000));
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

    /* ---- 5. Вогнутая кривая: интерьерный пик, верх убыточен ---- */
    {
        tb_arb_cfg_t c = mk_cfg();
        /* продажа x% = 130 − amt/1000: на 1000 → 129% (плюс), пик
         * ~15000, на 128000 → 2% (глубокий минус) — окно не дотягивается
         * до верха контракта */
        curve_t cv = { .num = 10, .den = 10, .snum = 100, .sden = 100,
                       .x_above = 130, .decay_num = 1, .decay_den = 1000 };
        CHECK(tb_arb_eval_pair(&c, &p, synth_quote, &cv, &r, &err) == 1);
        CHECK(r.ok);
        CHECK(r.size.w[0] == 1000); /* безубыточность на полу */
        /* пик внутри диапазона, а не на верхе */
        CHECK(r.opt_size.w[0] > 10000);
        CHECK(r.opt_size.w[0] < 20000);
        CHECK(r.opt_size.w[0] < 128000);
        /* прибыль на оптимуме больше, чем на безубыточности */
        {
            tb_u256_t at_min = tb_u256_from_u64(
                1000 * 129 / 100 - 1000 - 10 - required(1000));
            CHECK(tb_u256_cmp(&r.opt_profit, &at_min) > 0);
        }
        /* оптимум честно лучше соседних пробуемых точек вдвое больше */
        CHECK(r.opt_profit.w[0] > 2000);
    }

    /* ---- 6. Живой газ пары дороже фолбэка: пара исчезает ---- */
    {
        tb_arb_cfg_t c = mk_cfg();
        curve_t cv = { .num = 11, .den = 10, .snum = 12, .sden = 11,
                       .x_below = 100, .x_above = 100 };
        tb_arb_pair_in_t g = p;
        g.gas_cost_wei = 100000; /* как с цепи: дороже всей маржи */
        CHECK(tb_arb_eval_pair(&c, &g, synth_quote, &cv, &r, &err) == 0);
        CHECK(!r.ok);
        CHECK(strstr(r.reason, "нет прибыли") != NULL);
        CHECK(r.gas_cost_wei == 100000); /* газ пары, не конфига */
    }

    /* ---- 7. Живой газ в норме: расчёт по нему, не по фолбэку ---- */
    {
        tb_arb_cfg_t c = mk_cfg();
        curve_t cv = { .num = 11, .den = 10, .snum = 12, .sden = 11,
                       .x_below = 100, .x_above = 100 };
        tb_arb_pair_in_t g = p;
        g.gas_cost_wei = 7;
        CHECK(tb_arb_eval_pair(&c, &g, synth_quote, &cv, &r, &err) == 1);
        CHECK(r.ok);
        CHECK(r.gas_cost_wei == 7);
        CHECK(r.opt_profit.w[0]
              == 128000 * 12 / 10 - 128000 - 7 - required(128000));
        CHECK(r.min_out.w[0] == 128000 + 7 + required(128000));
    }

    printf("OK test_decision\n");
    return 0;
}
