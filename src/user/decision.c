#define TB_CURRENT_GROUP TB_G_DECISION /* зона ответственности файла */
/* ==========================================================================
 *  tb/user/decision.c — Decision Engine: глобальный поиск по паре пулов
 *  (чистая функция, котировки через инъекцию).
 *
 *    profit(size) = sell(buy(size)) − size − gas − required(size)
 *    required(size) = min_profit + size × TB_ARB_RISK_BP/10000
 *
 *  НЕТ предположения об одном интервале прибыльности (p0.1-C): кривая
 *  AMM может иметь несколько разрозненных прибыльных окон (нелинейность,
 *  ступени комиссий, ликвидность, отказы котировщика на отдельных
 *  размерах). Два уровня:
 *
 *  Уровень 1 — глобальная сетка TB_DEC_SCAN_POINTS геометрических точек
 *  по [floor, cap]: оценить каждую, увидеть ВСЕ смены знака и локальные
 *  максимумы. Ошибка котировщика в точке = «нет данных» (rc=0): точка
 *  выпадает, скан продолжается; ошиблись все — ошибка пары.
 *
 *  Уровень 2 — уточнение каждого найденного интервала: левая граница
 *  безубыточности бином, локальный максимум терном — до относительной
 *  ширины скобки ≤ TB_DEC_WIDTH_PCT или исчерпания бюджета проб.
 *  Результат: глобальный минимум входа + глобальный максимум прибыли.
 * ========================================================================== */
#include "tb/user/arb.h"
#include "tb/core/detail.h"

#include <stdio.h>
#include <string.h>

#define TB_DEC_SCAN_POINTS 8  /* уровень 1: точки сетки (включая края) */
#define TB_DEC_PROBE_BUDGET 36 /* общий бюджет проб на пару; каждая
                                * проба — 2 eth_call */
#define TB_DEC_WIDTH_PCT 5     /* уточнение до ≤5% относительной ширины */

/* Результат одной оценки: вердикт + числа при прибыльности. */
typedef struct probe_res {
    int       rc; /* 1 прибыльно, 0 нет, -1 ошибка котировщика */
    tb_u256_t buy_out;
    tb_u256_t sell_out;
    tb_u256_t size;      /* размер пробы (восстановление лучшего) */
    tb_u256_t profit; /* чистая прибыль (значимо при rc == 1) */
} probe_res_t;

/* Газ цикла: с цепи (pair_in) либо конфиг-фолбэк. */
static uint64_t pair_gas_wei(const tb_arb_cfg_t *cfg,
                             const tb_arb_pair_in_t *p)
{
    if (p->gas_cost_wei)
        return p->gas_cost_wei;
    return cfg->gas_est_wei ? cfg->gas_est_wei : 200000000000000ull;
}

/* Одна оценка: цепная котировка size через покупку и продажу.
 * Хаб — weth снапшотов (мультичейн: у каждой цепи свой). */
static int eval_size(const tb_arb_cfg_t *cfg, const tb_arb_pair_in_t *p,
                     tb_arb_quote_fn quote, void *qctx, const tb_u256_t *size,
                     probe_res_t *pr, tb_error_t *err)
{
    memset(pr, 0, sizeof(*pr));
    if (quote(qctx, &p->buy->pool, p->buy->fee, &p->buy->weth,
              &p->buy->token, size, &pr->buy_out, err) != 0)
        return -1;
    if (quote(qctx, &p->sell->pool, p->sell->fee, &p->sell->token,
              &p->sell->weth, &pr->buy_out, &pr->sell_out, err) != 0)
        return -1;

    /* profit = sell_out − size − gas − (min_profit + size×RISK/10000) */
    tb_u256_t acc = pr->sell_out;
    if (!tb_u256_sub(&acc, &acc, size))
        return 0; /* sell_out < size: глубоко в минусе */
    tb_u256_t gas = tb_u256_from_u64(pair_gas_wei(cfg, p));
    if (!tb_u256_sub(&acc, &acc, &gas))
        return 0;
    tb_u256_t req = tb_u256_from_u64(
        cfg->min_profit_wei ? cfg->min_profit_wei : 10000000000000ull);
    tb_u256_t risk;
    if (!tb_u256_mul_u64(&risk, size, TB_ARB_RISK_BP))
        return 0;
    (void)tb_u256_div_u64(&risk, &risk, 10000);
    if (!tb_u256_add(&req, &req, &risk))
        return 0;
    if (!tb_u256_sub(&acc, &acc, &req))
        return 0; /* минус — не ошибка, просто невыгодно */
    pr->profit = acc;
    pr->rc = tb_u256_is_zero(&acc) ? 0 : 1;
    return pr->rc;
}

/* Геометрическая сетка [floor, cap]: наименьший целый множитель s >= 2,
 * при котором floor × s^(SCAN-1) >= cap; точки floor×s^k, последняя —
 * ровно cap (дубли невозможны: перед cap обрываем). Целочисленно. */
static uint64_t grid_step(const tb_u256_t *floor, const tb_u256_t *cap)
{
    unsigned __int128 top = (unsigned __int128)cap->w[0]
                            | ((unsigned __int128)cap->w[1] << 64);
    for (uint64_t s = 2; s < 1024; s++) {
        unsigned __int128 cur = (unsigned __int128)floor->w[0]
                                | ((unsigned __int128)floor->w[1] << 64);
        for (int k = 0; k < TB_DEC_SCAN_POINTS - 1; k++) {
            cur *= s;
            if (cur >= top)
                return s;
        }
    }
    return 1024;
}

/* Одна проба с учётом бюджета; 0 — проба сделана, -1 — бюджет исчерпан */
static int probe(const tb_arb_cfg_t *cfg, const tb_arb_pair_in_t *p,
                 tb_arb_quote_fn quote, void *qctx, const tb_u256_t *size,
                 probe_res_t *pr, uint32_t *probes, tb_error_t *err)
{
    if (*probes >= TB_DEC_PROBE_BUDGET)
        return -1;
    int rc = eval_size(cfg, p, quote, qctx, size, pr, err);
    (*probes)++;
    pr->size = *size;
    return rc;
}

/* Бином до относительной ширины ≤ WIDTH_PCT: [a] нерентабельно,
 * [b] рентабельно; уточняем минимальное рентабельное b. Возвращает
 * лучшее (минимальное) рентабельное значение и его результат. */
static void refine_entry(const tb_arb_cfg_t *cfg, const tb_arb_pair_in_t *p,
                         tb_arb_quote_fn quote, void *qctx,
                         tb_u256_t a, tb_u256_t b, probe_res_t *best,
                         uint32_t *probes, tb_error_t *err)
{
    probe_res_t pr;
    for (;;) {
        /* ширина ≤ 5% от a — достаточная точность входа */
        tb_u256_t width;
        if (!tb_u256_sub(&width, &b, &a))
            return;
        tb_u256_t rel = a;
        (void)tb_u256_mul_u64(&rel, &rel, TB_DEC_WIDTH_PCT);
        (void)tb_u256_div_u64(&rel, &rel, 100);
        if (tb_u256_cmp(&width, &rel) <= 0)
            return;
        tb_u256_t mid;
        if (!tb_u256_add(&mid, &a, &b)
            || !tb_u256_div_u64(&mid, &mid, 2))
            return;
        if (tb_u256_cmp(&mid, &a) <= 0 || tb_u256_cmp(&mid, &b) >= 0)
            return;
        if (probe(cfg, p, quote, qctx, &mid, &pr, probes, err) < 0)
            return; /* ошибка котировщика при уточнении — оставляем
                     * лучшее известное (скан уже дал границы) */
        if (pr.rc == 1) {
            b = mid;
            *best = pr; /* минимальное рентабельное пока — это b */
        } else {
            a = mid;
        }
    }
}

/* Локальный максимум терном в [lo, hi]; best — лучшее известное внутри
 * (инициализирован точками сетки). Уточняет до ширины ≤ WIDTH_PCT. */
static void refine_peak(const tb_arb_cfg_t *cfg, const tb_arb_pair_in_t *p,
                        tb_arb_quote_fn quote, void *qctx,
                        tb_u256_t lo, tb_u256_t hi, probe_res_t *best,
                        uint32_t *probes, tb_error_t *err)
{
    probe_res_t r1, r2;
    for (;;) {
        tb_u256_t width;
        if (!tb_u256_sub(&width, &hi, &lo))
            return;
        tb_u256_t rel = lo;
        (void)tb_u256_mul_u64(&rel, &rel, TB_DEC_WIDTH_PCT);
        (void)tb_u256_div_u64(&rel, &rel, 100);
        if (tb_u256_cmp(&width, &rel) <= 0)
            return;
        tb_u256_t third = width;
        (void)tb_u256_div_u64(&third, &third, 3);
        if (tb_u256_is_zero(&third))
            return;
        tb_u256_t m1, m2;
        if (!tb_u256_add(&m1, &lo, &third)
            || !tb_u256_sub(&m2, &hi, &third))
            return;
        if (tb_u256_cmp(&m1, &lo) <= 0 || tb_u256_cmp(&m2, &m1) <= 0
            || tb_u256_cmp(&hi, &m2) <= 0)
            return;
        if (probe(cfg, p, quote, qctx, &m1, &r1, probes, err) < 0
            || probe(cfg, p, quote, qctx, &m2, &r2, probes, err) < 0)
            return;
        if (r1.rc == 1 && tb_u256_cmp(&r1.profit, &best->profit) > 0)
            *best = r1;
        if (r2.rc == 1 && tb_u256_cmp(&r2.profit, &best->profit) > 0)
            *best = r2;
        tb_u256_t p1 = r1.rc == 1 ? r1.profit : tb_u256_from_u64(0);
        tb_u256_t p2 = r2.rc == 1 ? r2.profit : tb_u256_from_u64(0);
        if (tb_u256_cmp(&p1, &p2) < 0)
            lo = m1;
        else
            hi = m2;
    }
}

int tb_arb_eval_pair(const tb_arb_cfg_t *cfg, const tb_arb_pair_in_t *p,
                     tb_arb_quote_fn quote, void *qctx,
                     tb_arb_pair_res_t *res, tb_error_t *err)
{
    memset(res, 0, sizeof(*res));
    res->gas_cost_wei = pair_gas_wei(cfg, p);

    /* пол поиска: ручной (совместимость) или точка безубыточности
     * движком от TB_ARB_SIZE_FLOOR_WEI; верх — абсолютный потолок */
    tb_u256_t lo = tb_u256_from_u64(
        cfg->min_trade_wei ? cfg->min_trade_wei : TB_ARB_SIZE_FLOOR_WEI);
    tb_u256_t hi = tb_u256_from_u64(
        cfg->max_trade_wei ? cfg->max_trade_wei : 10000000000000000ull);
    if (tb_u256_cmp(&lo, &hi) > 0 || tb_u256_is_zero(&lo)) {
        snprintf(res->reason, sizeof(res->reason),
                 "конфиг: пол поиска > верха контракта");
        return 0;
    }

    /* траектория поиска (капсула деталей на ошибке котировщика) */
    uint64_t t_size[3] = {0}, t_out[3] = {0};
    int t_n = 0;
#define TRACK(sz, so)                                                          \
    do {                                                                       \
        if (t_n < 3) {                                                         \
            t_size[t_n] = (sz).w[0];                                           \
            t_out[t_n] = (so).w[0];                                            \
            t_n++;                                                             \
        }                                                                      \
    } while (0)
#define ATTACH_CURVE()                                                         \
    do {                                                                       \
        uint8_t pack[TB_DETAIL_DATA_MAX];                                      \
        size_t plen = tb_detail_pack_curve(pack, sizeof(pack), t_size, t_out); \
        tb_error_detail(err, TB_G_DECISION, TB_CMD_DECIDE_CURVE, pack, plen);  \
    } while (0)

    /* ---- уровень 1: глобальная сетка ---- */
    tb_u256_t grid[TB_DEC_SCAN_POINTS];
    probe_res_t gp[TB_DEC_SCAN_POINTS];
    int grid_n = 0;
    uint32_t probes = 0;
    uint32_t grid_err = 0;

    uint64_t s = grid_step(&lo, &hi);
    tb_u256_t cur = lo;
    for (int k = 0; k < TB_DEC_SCAN_POINTS && grid_n < TB_DEC_SCAN_POINTS;
         k++) {
        tb_u256_t amt = cur;
        if (k > 0) {
            if (!tb_u256_mul_u64(&amt, &amt, s))
                amt = hi;
            if (tb_u256_cmp(&amt, &hi) >= 0)
                amt = hi; /* последняя точка — ровно cap */
        }
        if (grid_n > 0 && tb_u256_cmp(&amt, &grid[grid_n - 1]) == 0)
            break; /* упёрлись в cap раньше — сетка короче */
        int rc = probe(cfg, p, quote, qctx, &amt, &gp[grid_n], &probes,
                       err);
        if (rc < 0) {
            grid_err++;
            gp[grid_n].rc = 0; /* нет данных = нерентабельная точка */
        }
        TRACK(amt, gp[grid_n].sell_out);
        grid[grid_n] = amt;
        grid_n++;
        if (tb_u256_cmp(&amt, &hi) == 0)
            break;
        cur = amt;
    }
    if (grid_err == (uint32_t)grid_n) {
        ATTACH_CURVE();
        return -1; /* котировщик не ответил ни на одну точку */
    }

    /* ---- разбор сетки: интервалы прибыльности ---- */
    struct interval {
        int lo_i, hi_i;   /* индексы рентабельных точек (включительно) */
        bool entry_bracket; /* слева скобка 0->1 (уточнить вход) */
        int lo_bracket_i;   /* индекс нерентабельной точки слева */
        bool exit_bracket;  /* справа скобка 1->0 (правая граница) */
        int hi_bracket_i;
    } ivs[TB_DEC_SCAN_POINTS];
    int ivs_n = 0;

    for (int i = 0; i < grid_n;) {
        if (gp[i].rc != 1) {
            i++;
            continue;
        }
        int j = i;
        while (j + 1 < grid_n && gp[j + 1].rc == 1)
            j++;
        struct interval iv = {
            .lo_i = i,
            .hi_i = j,
            .entry_bracket = i > 0 && gp[i - 1].rc == 0,
            .lo_bracket_i = i > 0 ? i - 1 : -1,
            .exit_bracket = j + 1 < grid_n && gp[j + 1].rc == 0,
            .hi_bracket_i = j + 1 < grid_n ? j + 1 : -1,
        };
        ivs[ivs_n++] = iv;
        i = j + 1;
    }
    if (ivs_n == 0) {
        snprintf(res->reason, sizeof(res->reason),
                 "нет прибыли в диапазоне");
        res->probes = probes;
        return 0;
    }

    /* ---- уровень 2: уточнение интервалов ----
     * сетка возрастает, интервалы идут слева направо: первый интервал
     * держит глобальный минимум входа; максимумы сравниваем все */
    tb_u256_t best_size = gp[ivs[0].lo_i].size;
    probe_res_t best_pr = gp[ivs[0].lo_i];
    tb_u256_t min_entry = grid[ivs[0].lo_i];
    probe_res_t min_pr = gp[ivs[0].lo_i];

    for (int v = 0; v < ivs_n; v++) {
        struct interval *iv = &ivs[v];

        /* левая граница безубыточности первого интервала */
        if (v == 0) {
            min_pr = gp[iv->lo_i];
            min_entry = grid[iv->lo_i];
            if (iv->entry_bracket) {
                refine_entry(cfg, p, quote, qctx, grid[iv->lo_bracket_i],
                             grid[iv->lo_i], &min_pr, &probes, err);
                min_entry = min_pr.size; /* уточнённый вход */
            }
        }

        /* локальный максимум: лучший из точек интервала … */
        probe_res_t loc_pr = gp[iv->lo_i];
        for (int k = iv->lo_i; k <= iv->hi_i; k++)
            if (tb_u256_cmp(&gp[k].profit, &loc_pr.profit) > 0)
                loc_pr = gp[k];
        /* … затем терн в скобке [лев, прав] (правая — скобка спада) */
        tb_u256_t pk_lo = grid[iv->lo_i];
        tb_u256_t pk_hi = grid[iv->hi_i];
        if (iv->exit_bracket)
            pk_hi = grid[iv->hi_bracket_i];
        refine_peak(cfg, p, quote, qctx, pk_lo, pk_hi, &loc_pr, &probes,
                    err);
        if (tb_u256_cmp(&loc_pr.profit, &best_pr.profit) > 0) {
            best_pr = loc_pr;
            best_size = loc_pr.size;
        }
    }

    /* ---- результат ---- */
    res->ok = true;
    res->size = min_entry;
    res->buy_out = min_pr.buy_out;
    res->sell_out = min_pr.sell_out;
    res->opt_size = best_size;
    res->opt_sell_out = best_pr.sell_out;
    res->opt_profit = best_pr.profit;
    res->probes = probes;

    /* min_out = opt_size + gas + min_profit + opt_size×RISK/10000:
     * исполнение ниже пола = чистая прибыль ниже требуемой → реверт */
    tb_u256_t risk;
    (void)tb_u256_mul_u64(&risk, &res->opt_size, TB_ARB_RISK_BP);
    (void)tb_u256_div_u64(&risk, &risk, 10000);
    tb_u256_t add = tb_u256_from_u64(
        (cfg->min_profit_wei ? cfg->min_profit_wei : 10000000000000ull));
    tb_u256_t gas = tb_u256_from_u64(res->gas_cost_wei);
    if (!tb_u256_add(&add, &add, &risk) || !tb_u256_add(&add, &add, &gas)
        || !tb_u256_add(&res->min_out, &res->opt_size, &add)) {
        res->ok = false;
        snprintf(res->reason, sizeof(res->reason), "переполнение min_out");
    }
    return 1;
}
