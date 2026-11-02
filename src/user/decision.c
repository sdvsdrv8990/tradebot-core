#define TB_CURRENT_GROUP TB_G_DECISION /* зона ответственности файла */
/* ==========================================================================
 *  tb/user/decision.c — Decision Engine: поиск минимальной выгодной
 *  ставки на паре пулов (чистая функция, котировки через инъекцию).
 *
 *  profit(size) = sell(quote_buy(size)) − size − gas_est − min_profit
 *
 *  Стратегия «торгуем на минималках»: ищем наименьший size из
 *  [min_trade, верх контракта], на котором profit > 0 — удвоением от
 *  минималки до первого прибыльного (максимум log2(cap/min) оценок),
 *  затем бинарное уточнение границ (3 шага). Если даже на верхе
 *  контракта прибыли нет — сделка не предлагается.
 * ========================================================================== */
#include "tb/user/arb.h"
#include "tb/core/detail.h"

#include <stdio.h>
#include <string.h>

/* Одна оценка: цепная котировка size через покупку и продажу.
 * Хаб — weth снапшотов (мультичейн: у каждой цепи свой). */
static int eval_size(const tb_arb_cfg_t *cfg, const tb_arb_pair_in_t *p,
                     tb_arb_quote_fn quote, void *qctx, const tb_u256_t *size,
                     tb_u256_t *buy_out, tb_u256_t *sell_out,
                     tb_error_t *err)
{
    if (quote(qctx, &p->buy->pool, p->buy->fee, &p->buy->weth,
              &p->buy->token, size, buy_out, err) != 0)
        return -1;
    if (quote(qctx, &p->sell->pool, p->sell->fee, &p->sell->token,
              &p->sell->weth, buy_out, sell_out, err) != 0)
        return -1;

    /* profit = sell_out − size − gas − min_profit */
    tb_u256_t acc = *sell_out, cost;
    if (!tb_u256_sub(&acc, &acc, size))
        return 0; /* sell_out < size: глубоко в минусе */
    tb_u256_t gas = tb_u256_from_u64(
        cfg->gas_est_wei ? cfg->gas_est_wei : 200000000000000ull);
    tb_u256_t mp = tb_u256_from_u64(
        cfg->min_profit_wei ? cfg->min_profit_wei : 10000000000000ull);
    if (!tb_u256_add(&cost, &gas, &mp))
        return 0;
    if (!tb_u256_sub(&acc, &acc, &cost))
        return 0; /* минус — не ошибка, просто невыгодно */
    return tb_u256_is_zero(&acc) ? 0 : 1;
}

int tb_arb_eval_pair(const tb_arb_cfg_t *cfg, const tb_arb_pair_in_t *p,
                     tb_arb_quote_fn quote, void *qctx,
                     tb_arb_pair_res_t *res, tb_error_t *err)
{
    memset(res, 0, sizeof(*res));

    tb_u256_t lo = tb_u256_from_u64(
        cfg->min_trade_wei ? cfg->min_trade_wei : 100000000000000ull);
    tb_u256_t hi = tb_u256_from_u64(
        cfg->max_trade_wei ? cfg->max_trade_wei : 10000000000000000ull);
    if (tb_u256_cmp(&lo, &hi) > 0 || tb_u256_is_zero(&lo)) {
        snprintf(res->reason, sizeof(res->reason),
                 "конфиг: min_trade > верха контракта");
        return 0;
    }

    tb_u256_t b1, s1;
    /* траектория поиска (для капсулы деталей на ошибке котировщика) */
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

    /* 1) на верхе контракта прибыли нет — не тратим RPC дальше */
    int rc = eval_size(cfg, p, quote, qctx, &hi, &b1, &s1, err);
    if (rc < 0) {
        ATTACH_CURVE();
        return -1;
    }
    TRACK(hi, s1);
    res->probes++;
    if (rc == 0) {
        snprintf(res->reason, sizeof(res->reason),
                 "нет прибыли на верхе контракта");
        return 0;
    }

    /* 2) на минималке прибыль есть — минималка и есть ставка */
    tb_u256_t size = lo;
    rc = eval_size(cfg, p, quote, qctx, &size, &b1, &s1, err);
    if (rc < 0) {
        ATTACH_CURVE();
        return -1;
    }
    TRACK(size, s1);
    res->probes++;
    if (rc == 1) {
        res->ok = true;
        res->size = lo;
        res->buy_out = b1;
        res->sell_out = s1;
        goto done;
    }

    /* 3) удвоение от минималки до первого прибыльного размера */
    tb_u256_t cur = lo, prev = lo;
    bool found = false;
    while (tb_u256_cmp(&cur, &hi) < 0) {
        prev = cur;
        if (!tb_u256_mul_u64(&cur, &cur, 2) || tb_u256_cmp(&cur, &hi) > 0)
            cur = hi;
        rc = eval_size(cfg, p, quote, qctx, &cur, &b1, &s1, err);
        if (rc < 0) {
            ATTACH_CURVE();
            return -1;
        }
        TRACK(cur, s1);
        res->probes++;
        if (rc == 1) {
            found = true;
            break;
        }
    }
    if (!found) {
        /* между lo и hi когда-то было выгодно на hi — противоречие
         * (кривая сместилась); честно фиксируем */
        snprintf(res->reason, sizeof(res->reason),
                 "прибыль исчезла в процессе поиска");
        return 0;
    }

    /* 4) бинарное уточнение: минимальный размер в (prev, cur] */
    for (int i = 0; i < 3; i++) {
        tb_u256_t mid;
        tb_u256_t two = tb_u256_from_u64(2);
        if (!tb_u256_add(&mid, &prev, &cur)
            || !tb_u256_div_u64(&mid, &mid, 2))
            break;
        if (tb_u256_cmp(&mid, &prev) <= 0 || tb_u256_cmp(&mid, &cur) >= 0)
            break;
        tb_u256_t bm, sm;
        rc = eval_size(cfg, p, quote, qctx, &mid, &bm, &sm, err);
        if (rc < 0)
            return -1;
        res->probes++;
        if (rc == 1)
            cur = mid;
        else
            prev = mid;
    }

    rc = eval_size(cfg, p, quote, qctx, &cur, &b1, &s1, err);
    if (rc < 0)
        return -1;
    res->probes++;
    if (rc != 1) {
        snprintf(res->reason, sizeof(res->reason),
                 "уточнение не сошлось (рынок движется)");
        return 0;
    }
    res->ok = true;
    res->size = cur;

done:
    res->buy_out = b1;
    res->sell_out = s1;
    /* min_out фиксируем при отправке: size + min_profit + gas */
    tb_u256_t add = tb_u256_from_u64(
        (cfg->min_profit_wei ? cfg->min_profit_wei : 10000000000000ull)
        + (cfg->gas_est_wei ? cfg->gas_est_wei : 200000000000000ull));
    if (!tb_u256_add(&res->min_out, &res->size, &add)) {
        res->ok = false;
        snprintf(res->reason, sizeof(res->reason), "переполнение min_out");
    }
    return 1;
}
