/* ==========================================================================
 *  tests/test_filters.c — фильтры 1–4 USER-домена на синтетических
 *  снапшотах: актуальность, ликвидность (влияние), безопасность
 *  (allowlist), волатильность (история мидов).
 * ========================================================================== */
#include <stdio.h>
#include <string.h>

#include "harness.h"
#include "tb/core/error.h"
#include "tb/user/arb.h"

static tb_u256_t u64(uint64_t v)
{
    return tb_u256_from_u64(v);
}

static tb_arb_snap_t mk_snap(uint64_t buy_out, uint64_t big_out,
                             const char *dex, uint64_t age_ms)
{
    tb_arb_snap_t s;
    memset(&s, 0, sizeof(s));
    s.buy_out = u64(buy_out);
    s.big_out = u64(big_out);
    s.sell_back = u64(buy_out);
    snprintf(s.dex, sizeof(s.dex), "%s", dex);
    s.ts_ms = 1000000 - age_ms; /* now = 1000000 в тестах */
    s.complete = true;
    return s;
}

static tb_arb_cfg_t mk_cfg(void)
{
    tb_arb_cfg_t c;
    memset(&c, 0, sizeof(c));
    c.poll_ms = 4000;            /* freshness по умолчанию: 12000 */
    c.liquidity_probe_mult = 10; /* big = probe*10 */
    c.liquidity_max_bp = 100;    /* 1% */
    c.volatility_max_bp = 300;   /* 3% */
    return c;
}

int main(void)
{
    /* ---- 1. Актуальность ---- */
    {
        tb_arb_cfg_t c = mk_cfg();
        tb_arb_hist_t h = {0};
        tb_arb_verdict_t v;
        tb_arb_snap_t s = mk_snap(1000, 9900, "dex-ok", 100);
        CHECK(tb_arb_filter_eval(&c, &s, &h, 1000000, &v) == 0);
        CHECK(v.pass && v.freshness_ok);

        tb_arb_snap_t stale = mk_snap(1000, 9900, "dex-ok", 13000);
        CHECK(tb_arb_filter_eval(&c, &stale, &h, 1000000, &v) == 0);
        CHECK(!v.pass && !v.freshness_ok);
        CHECK(strstr(v.reject, "актуальность") != NULL);
    }

    /* ---- 2. Ликвидность: impact_bp = (buy*mult − big)*10000/(buy*mult) ---- */
    {
        tb_arb_cfg_t c = mk_cfg();
        tb_arb_hist_t h = {0};
        tb_arb_verdict_t v;

        /* пропорционально: 1000 -> 10000, impact 0 */
        tb_arb_snap_t deep = mk_snap(1000, 10000, "dex-ok", 0);
        CHECK(tb_arb_filter_eval(&c, &deep, &h, 1000000, &v) == 0);
        CHECK(v.pass && v.impact_bp == 0);

        /* 1000 -> 9700: impact = 3000*10000/10000 = 300 б.п. > 100 */
        tb_arb_snap_t thin = mk_snap(1000, 9700, "dex-ok", 0);
        CHECK(tb_arb_filter_eval(&c, &thin, &h, 1000000, &v) == 0);
        CHECK(!v.pass && !v.liquidity_ok && v.impact_bp == 300);
        CHECK(strstr(v.reject, "ликвидность") != NULL);

        /* 1000 -> 9960: 40 б.п. — ок */
        tb_arb_snap_t mid = mk_snap(1000, 9960, "dex-ok", 0);
        CHECK(tb_arb_filter_eval(&c, &mid, &h, 1000000, &v) == 0);
        CHECK(v.pass && v.impact_bp == 40);
    }

    /* ---- 3. Безопасность: allowlist топ-N ---- */
    {
        tb_arb_cfg_t c = mk_cfg();
        strcpy(c.dex_allow[0], "uniswap-v3");
        strcpy(c.dex_allow[1], "pancakeswap-v3");
        c.dex_allow_n = 2;
        tb_arb_hist_t h = {0};
        tb_arb_verdict_t v;

        tb_arb_snap_t good = mk_snap(1000, 9900, "uniswap-v3", 0);
        CHECK(tb_arb_filter_eval(&c, &good, &h, 1000000, &v) == 0);
        CHECK(v.pass && v.safety_ok);

        tb_arb_snap_t bad = mk_snap(1000, 9900, "scam-dex", 0);
        CHECK(tb_arb_filter_eval(&c, &bad, &h, 1000000, &v) == 0);
        CHECK(!v.pass && !v.safety_ok);
        CHECK(strstr(v.reject, "безопасность") != NULL);

        /* allowlist не задан — не проверяем */
        c.dex_allow_n = 0;
        CHECK(tb_arb_filter_eval(&c, &bad, &h, 1000000, &v) == 0);
        CHECK(v.pass);
    }

    /* ---- 4. Волатильность: максимальное отклонение от среднего ---- */
    {
        tb_arb_cfg_t c = mk_cfg();
        tb_arb_verdict_t v;

        /* стабильная история: 1000..1010, среднее ~1005, отклонение ~0.5% */
        tb_arb_hist_t stable = {0};
        for (int i = 0; i < 6; i++) {
            tb_u256_t mid = u64(1000 + i * 2);
            tb_arb_hist_push(&stable, &mid);
        }
        tb_arb_snap_t s = mk_snap(1010, 10050, "dex-ok", 0);
        CHECK(tb_arb_filter_eval(&c, &s, &stable, 1000000, &v) == 0);
        CHECK(v.pass && v.volatility_ok && v.vol_bp <= 300);

        /* дикая история: один выброс 2000 среди 1000 */
        tb_arb_hist_t wild = {0};
        for (int i = 0; i < 5; i++) {
            tb_u256_t mid = u64(1000);
            tb_arb_hist_push(&wild, &mid);
        }
        tb_u256_t spike = u64(2000);
        tb_arb_hist_push(&wild, &spike);
        /* среднее ~1166, отклонение 833/1166 = 7140 б.п. */
        CHECK(tb_arb_filter_eval(&c, &s, &wild, 1000000, &v) == 0);
        CHECK(!v.pass && !v.volatility_ok);
        CHECK(strstr(v.reject, "волатильность") != NULL);

        /* мало данных (< 3) — не топим */
        tb_arb_hist_t young = {0};
        tb_u256_t one = u64(1000);
        tb_arb_hist_push(&young, &one);
        CHECK(tb_arb_filter_eval(&c, &s, &young, 1000000, &v) == 0);
        CHECK(v.pass);

        /* кольцевой буфер: после 8 вставок старые вытесняются */
        tb_arb_hist_t ring = {0};
        tb_u256_t flat = u64(1000), bump = u64(1002);
        for (int i = 0; i < TB_ARB_HISTORY_LEN + 2; i++)
            tb_arb_hist_push(&ring, &flat);
        tb_arb_hist_push(&ring, &bump);
        CHECK(ring.n == TB_ARB_HISTORY_LEN);
    }

    /* ---- 0. Контракт контекста: неполный снапшот отсекается явно ---- */
    {
        tb_arb_cfg_t c = mk_cfg();
        tb_arb_hist_t h = {0};
        tb_arb_verdict_t v;
        tb_arb_snap_t broken = mk_snap(1000, 9900, "dex-ok", 0);
        broken.complete = false; /* котировки не донесены */
        CHECK(tb_arb_filter_eval(&c, &broken, &h, 1000000, &v) == 0);
        CHECK(!v.pass);
        CHECK(strstr(v.reject, "неполный") != NULL);
        CHECK(!v.freshness_ok && !v.liquidity_ok); /* проверки не шли */
    }

    printf("OK test_filters\n");
    return 0;
}
