#define TB_CURRENT_GROUP TB_G_DECISION /* зона ответственности файла */
/* ==========================================================================
 *  tb/user/decision.c — Decision Engine: две фазы поиска ставки на паре
 *  пулов (чистая функция, котировки через инъекцию).
 *
 *    profit(size) = sell(buy(size)) − size − gas − required(size)
 *    required(size) = min_profit + size × TB_ARB_RISK_BP/10000
 *
 *  Фаза A — точка безубыточности: минимальный size из [пол, верх
 *  контракта] с profit > 0. Быстрый путь: profit есть на верху —
 *  удвоение от пола до первого прибыльного + бинарное уточнение.
 *  Если верх убыточен (окно не дотягивается до потолка) —
 *  геометрическая лестница внутри диапазона ловит интерьерные окна.
 *
 *  Фаза B — optimum: пик кривой profit (комиссии+влияние гасят рост).
 *  Удвоение от минимума до первого спада, затем тернарное уточнение
 *  скобки. Ордер подаётся на opt_size; profit при нём — opt_profit.
 *  ========================================================================== */
#include "tb/user/arb.h"
#include "tb/core/detail.h"

#include <stdio.h>
#include <string.h>

#define TB_DEC_REFINE_ITERS 3  /* бинарных/тернарных шагов уточнения */
#define TB_DEC_LADDER_STEPS 6  /* проб лестницы внутри (пол, верх) */

/* Результат одной оценки: вердикт + числа при прибыльности. */
typedef struct probe_res {
    int       rc; /* 1 прибыльно, 0 нет, -1 ошибка котировщика */
    tb_u256_t buy_out;
    tb_u256_t sell_out;
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
    if (!tb_u256_sub(&acc, &acc, size)) {
        return 0; /* sell_out < size: глубоко в минусе */
    }
    tb_u256_t gas = tb_u256_from_u64(pair_gas_wei(cfg, p));
    if (!tb_u256_sub(&acc, &acc, &gas)) {
        return 0;
    }
    tb_u256_t req = tb_u256_from_u64(
        cfg->min_profit_wei ? cfg->min_profit_wei : 10000000000000ull);
    tb_u256_t risk;
    if (!tb_u256_mul_u64(&risk, size, TB_ARB_RISK_BP)) {
        return 0;
    }
    (void)tb_u256_div_u64(&risk, &risk, 10000);
    if (!tb_u256_add(&req, &req, &risk))
        return 0;
    if (!tb_u256_sub(&acc, &acc, &req)) {
        return 0; /* минус — не ошибка, просто невыгодно */
    }
    pr->profit = acc;
    pr->rc = tb_u256_is_zero(&acc) ? 0 : 1;
    return pr->rc;
}

/* Лестница геометрических проб внутри (lo, hi): наименьший целый
 * множитель s >= 2, при котором lo × s^(STEPS+1) >= hi (точки
 * lo×s^k, k=1..STEPS). Целочисленно (без float): детерминизм тестов. */
static uint64_t ladder_step(const tb_u256_t *lo, const tb_u256_t *hi)
{
    unsigned __int128 top = (unsigned __int128)hi->w[0]
                            | ((unsigned __int128)hi->w[1] << 64);
    for (uint64_t s = 2; s < 1024; s++) {
        unsigned __int128 cur = (unsigned __int128)lo->w[0]
                                | ((unsigned __int128)lo->w[1] << 64);
        for (int k = 0; k <= TB_DEC_LADDER_STEPS; k++) {
            cur *= s;
            if (cur >= top)
                return s;
        }
    }
    return 1024; /* диапазон шире любой лестницы: крупный шаг */
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

    probe_res_t pr;

    /* ---- фаза A: точка безубыточности ---- */

    /* 1) быстрый гейт: прибыль на верху контракта */
    if (eval_size(cfg, p, quote, qctx, &hi, &pr, err) < 0) {
        ATTACH_CURVE();
        return -1;
    }
    TRACK(hi, pr.sell_out);
    res->probes++;

    tb_u256_t min_size;
    probe_res_t min_pr;

    if (pr.rc == 1) {
        /* 2) прибыль на самом полу — пол и есть точка безубыточности */
        if (eval_size(cfg, p, quote, qctx, &lo, &pr, err) < 0) {
            ATTACH_CURVE();
            return -1;
        }
        TRACK(lo, pr.sell_out);
        res->probes++;
        if (pr.rc == 1) {
            min_size = lo;
            min_pr = pr;
            goto have_min;
        }
        /* 3) удвоение от пола до первого прибыльного */
        tb_u256_t cur = lo, prev = lo;
        bool found = false;
        while (tb_u256_cmp(&cur, &hi) < 0) {
            prev = cur;
            if (!tb_u256_mul_u64(&cur, &cur, 2) || tb_u256_cmp(&cur, &hi) > 0)
                cur = hi;
            if (eval_size(cfg, p, quote, qctx, &cur, &pr, err) < 0) {
                ATTACH_CURVE();
                return -1;
            }
            TRACK(cur, pr.sell_out);
            res->probes++;
            if (pr.rc == 1) {
                found = true;
                break;
            }
        }
        if (!found) {
            /* прибыль была на верху, но исчезла в процессе — кривая
             * сместилась; фиксируем честно */
            snprintf(res->reason, sizeof(res->reason),
                     "прибыль исчезла в процессе поиска");
            return 0;
        }
        /* 4) бинарное уточнение: минимум в (prev, cur] */
        for (int i = 0; i < TB_DEC_REFINE_ITERS; i++) {
            tb_u256_t mid;
            if (!tb_u256_add(&mid, &prev, &cur)
                || !tb_u256_div_u64(&mid, &mid, 2))
                break;
            if (tb_u256_cmp(&mid, &prev) <= 0 || tb_u256_cmp(&mid, &cur) >= 0)
                break;
            probe_res_t mr;
            if (eval_size(cfg, p, quote, qctx, &mid, &mr, err) < 0) {
                ATTACH_CURVE();
                return -1;
            }
            res->probes++;
            if (mr.rc == 1)
                cur = mid;
            else
                prev = mid;
        }
        if (eval_size(cfg, p, quote, qctx, &cur, &pr, err) < 0) {
            ATTACH_CURVE();
            return -1;
        }
        res->probes++;
        if (pr.rc != 1) {
            snprintf(res->reason, sizeof(res->reason),
                     "уточнение не сошлось (рынок движется)");
            return 0;
        }
        min_size = cur;
        min_pr = pr;
    } else {
        /* верх убыточен: интерьерное окно? лестница геометрических проб
         * (пол уже пробуем — он дешевле всех) */
        if (eval_size(cfg, p, quote, qctx, &lo, &pr, err) < 0) {
            ATTACH_CURVE();
            return -1;
        }
        TRACK(lo, pr.sell_out);
        res->probes++;
        if (pr.rc == 1) {
            min_size = lo;
            min_pr = pr;
            goto have_min;
        }
        uint64_t s = ladder_step(&lo, &hi);
        tb_u256_t prev = lo, cur = lo;
        bool found = false;
        for (int k = 1; k <= TB_DEC_LADDER_STEPS; k++) {
            tb_u256_t amt = lo;
            bool over = false;
            for (int m = 0; m < k; m++)
                if (!tb_u256_mul_u64(&amt, &amt, s)) {
                    over = true;
                    break;
                }
            if (over || tb_u256_cmp(&amt, &hi) >= 0)
                break; /* верх уже пробован и убыточен — дальше нет смысла */
            if (tb_u256_cmp(&amt, &prev) == 0)
                continue;
            if (eval_size(cfg, p, quote, qctx, &amt, &pr, err) < 0) {
                ATTACH_CURVE();
                return -1;
            }
            TRACK(amt, pr.sell_out);
            res->probes++;
            cur = amt;
            if (pr.rc == 1) {
                found = true;
                break;
            }
            prev = amt;
        }
        if (!found) {
            /* reason[80] байт: кириллица 2 байт/символ — строка короче
             * буфера, иначе обрезка режет UTF-8 посередине символа */
            snprintf(res->reason, sizeof(res->reason),
                     "нет прибыли в диапазоне");
            return 0;
        }
        /* уточнение точки безубыточности в (prev, cur] */
        for (int i = 0; i < TB_DEC_REFINE_ITERS; i++) {
            tb_u256_t mid;
            if (!tb_u256_add(&mid, &prev, &cur)
                || !tb_u256_div_u64(&mid, &mid, 2))
                break;
            if (tb_u256_cmp(&mid, &prev) <= 0 || tb_u256_cmp(&mid, &cur) >= 0)
                break;
            probe_res_t mr;
            if (eval_size(cfg, p, quote, qctx, &mid, &mr, err) < 0) {
                ATTACH_CURVE();
                return -1;
            }
            res->probes++;
            if (mr.rc == 1)
                cur = mid;
            else
                prev = mid;
        }
        if (eval_size(cfg, p, quote, qctx, &cur, &pr, err) < 0) {
            ATTACH_CURVE();
            return -1;
        }
        res->probes++;
        if (pr.rc != 1) {
            snprintf(res->reason, sizeof(res->reason),
                     "уточнение не сошлось (рынок движется)");
            return 0;
        }
        min_size = cur;
        min_pr = pr;
    }

have_min:;

    /* ---- фаза B: optimum — пик кривой прибыли ---- */
    tb_u256_t best_size = min_size;
    probe_res_t best_pr = min_pr;

    /* скобка: удвоение от минимума до первого спада прибыли */
    tb_u256_t step = min_size, bracket_lo = min_size,
              bracket_hi = min_size;
    for (;;) {
        tb_u256_t next = step;
        bool capped = false;
        if (!tb_u256_mul_u64(&next, &next, 2) || tb_u256_cmp(&next, &hi) > 0) {
            next = hi;
            capped = true;
        }
        if (tb_u256_cmp(&next, &step) == 0)
            break; /* шаг уже на верхе */
        if (eval_size(cfg, p, quote, qctx, &next, &pr, err) < 0) {
            ATTACH_CURVE();
            return -1;
        }
        res->probes++;
        if (pr.rc == 1 && tb_u256_cmp(&pr.profit, &best_pr.profit) > 0) {
            bracket_lo = step; /* пик правее: скобка растёт вместе */
            step = next;
            best_size = next;
            best_pr = pr;
            bracket_hi = next;
            if (capped)
                break; /* прибыль растёт до самого верха: пик = верх */
            continue;
        }
        bracket_hi = next; /* спад: пик в (bracket_lo, next) */
        break;
    }

    /* тернарное уточнение скобки (best уже внутри) */
    for (int i = 0; i < TB_DEC_REFINE_ITERS
                    && tb_u256_cmp(&bracket_lo, &bracket_hi) < 0;
         i++) {
        tb_u256_t m1, m2, span;
        if (!tb_u256_sub(&span, &bracket_hi, &bracket_lo))
            break;
        tb_u256_t third = span;
        (void)tb_u256_div_u64(&third, &third, 3);
        if (tb_u256_is_zero(&third))
            break;
        if (!tb_u256_add(&m1, &bracket_lo, &third)
            || !tb_u256_sub(&m2, &bracket_hi, &third))
            break;
        probe_res_t r1, r2;
        int rc1 = eval_size(cfg, p, quote, qctx, &m1, &r1, err);
        if (rc1 < 0) {
            ATTACH_CURVE();
            return -1;
        }
        res->probes++;
        int rc2 = eval_size(cfg, p, quote, qctx, &m2, &r2, err);
        if (rc2 < 0) {
            ATTACH_CURVE();
            return -1;
        }
        res->probes++;
        if (r1.rc == 1 && tb_u256_cmp(&r1.profit, &best_pr.profit) > 0) {
            best_size = m1;
            best_pr = r1;
        }
        if (r2.rc == 1 && tb_u256_cmp(&r2.profit, &best_pr.profit) > 0) {
            best_size = m2;
            best_pr = r2;
        }
        tb_u256_t p1 = r1.rc == 1 ? r1.profit : tb_u256_from_u64(0);
        tb_u256_t p2 = r2.rc == 1 ? r2.profit : tb_u256_from_u64(0);
        if (tb_u256_cmp(&p1, &p2) < 0)
            bracket_lo = m1;
        else
            bracket_hi = m2;
    }

    /* ---- результат: безубыточность + оптимум, пол ордера на оптимум ---- */
    res->ok = true;
    res->size = min_size;
    res->buy_out = min_pr.buy_out;
    res->sell_out = min_pr.sell_out;
    res->opt_size = best_size;
    res->opt_sell_out = best_pr.sell_out;
    res->opt_profit = best_pr.profit;

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
