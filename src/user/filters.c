#define TB_CURRENT_GROUP TB_G_FILTERS /* зона ответственности файла */
/* ==========================================================================
 *  tb/user/filters.c — фильтры 1–4 USER-домена (чистые функции).
 *
 *    1 актуальность — возраст снапшота против freshness_max_ms;
 *    2 ликвидность  — расхождение ставки probe против probe*mult
 *                     (ценовое влияние растёт с размером: во сколько
 *                     теряем на большом размере, в б.п.);
 *    3 безопасность — пул из watchlist (гарантировано сборщиком) и
 *                     dex в allowlist (топ-N рейтинга);
 *    4 волатильность— максимальное отклонение мидов истории от
 *                     среднего, в б.п. (мало данных — пропускаем).
 *
 *  Отказ фильтра — не ошибка: снапшот исключается из рассмотрения,
 *  вердикт уходит в запись decision.evaluated (атрибуция влияния
 *  источников на решение).
 * ========================================================================== */
#include "tb/user/arb.h"

#include <string.h>

/* Деление a/b в б.п.: (a*10000)/b, пока b не влезает в u64 — сдвигаем
 * оба вправо на 64 бита (точность целых б.п. достаточна). */
static bool ratio_bp(const tb_u256_t *a, const tb_u256_t *b, uint32_t *out_bp)
{
    tb_u256_t num = *a, den = *b;
    if (tb_u256_is_zero(&den))
        return false;
    while (den.w[1] || den.w[2] || den.w[3]) {
        /* shr 64 */
        num.w[0] = num.w[1]; num.w[1] = num.w[2]; num.w[2] = num.w[3];
        num.w[3] = 0;
        den.w[0] = den.w[1]; den.w[1] = den.w[2]; den.w[2] = den.w[3];
        den.w[3] = 0;
        if (tb_u256_is_zero(&den))
            return false;
    }
    if (!tb_u256_mul_u64(&num, &num, 10000))
        return false;
    (void)tb_u256_div_u64(&num, &num, den.w[0]); /* остаток — усечение вниз */
    if (num.w[1] || num.w[2] || num.w[3] || num.w[0] > 0xffffffffull)
        return false; /* за пределами разумного bp — данные безумны */
    *out_bp = (uint32_t)num.w[0];
    return true;
}

/* Ставка на probe: buy_out; на probe*mult: big_out/mult.
 * impact_bp = (buy_out*mult − big_out) / (buy_out*mult) в б.п. */
static bool snap_impact_bp(const tb_arb_cfg_t *cfg, const tb_arb_snap_t *s,
                           uint32_t *out_bp)
{
    uint64_t mult = cfg->liquidity_probe_mult ? cfg->liquidity_probe_mult : 10;
    tb_u256_t small_scaled, diff;

    if (!tb_u256_mul_u64(&small_scaled, &s->buy_out, mult))
        return false;
    if (tb_u256_cmp(&s->big_out, &small_scaled) >= 0) {
        *out_bp = 0; /* большой размер не хуже пропорционального — не бывает
                        на V3, но защищаемся */
        return true;
    }
    if (!tb_u256_sub(&diff, &small_scaled, &s->big_out))
        return false;
    return ratio_bp(&diff, &small_scaled, out_bp);
}

static bool hist_vol_bp(const tb_arb_hist_t *h, uint32_t *out_bp)
{
    if (h->n < 3) {
        *out_bp = 0; /* недостаточно истории — не топим */
        return true;
    }

    tb_u256_t sum = tb_u256_from_u64(0);
    for (uint32_t i = 0; i < h->n; i++)
        if (!tb_u256_add(&sum, &sum, &h->buf[i]))
            return false;

    /* mean = sum / n; max отклонение от mean в б.п. */
    tb_u256_t mean = sum;
    (void)tb_u256_div_u64(&mean, &mean, h->n);

    tb_u256_t max_dev = tb_u256_from_u64(0);
    for (uint32_t i = 0; i < h->n; i++) {
        tb_u256_t dev;
        if (tb_u256_cmp(&h->buf[i], &mean) >= 0) {
            if (!tb_u256_sub(&dev, &h->buf[i], &mean))
                return false;
        } else {
            if (!tb_u256_sub(&dev, &mean, &h->buf[i]))
                return false;
        }
        if (tb_u256_cmp(&dev, &max_dev) > 0)
            max_dev = dev;
    }

    return ratio_bp(&max_dev, &mean, out_bp);
}

int tb_arb_filter_eval(const tb_arb_cfg_t *cfg, const tb_arb_snap_t *s,
                       const tb_arb_hist_t *h, uint64_t now_ms,
                       tb_arb_verdict_t *v)
{
    memset(v, 0, sizeof(*v));
    v->pass = true;

    /* 1. Актуальность */
    v->age_ms = now_ms > s->ts_ms ? now_ms - s->ts_ms : 0;
    uint64_t fmax = cfg->freshness_max_ms;
    if (fmax == 0)
        fmax = cfg->poll_ms ? 3 * cfg->poll_ms : 24000;
    v->freshness_ok = v->age_ms <= fmax;
    if (!v->freshness_ok) {
        v->pass = false;
        snprintf(v->reject, sizeof(v->reject), "актуальность: %llu мс > %llu",
                 (unsigned long long)v->age_ms, (unsigned long long)fmax);
        return 0; /* первая причина — достаточна */
    }

    /* 2. Ликвидность */
    if (!snap_impact_bp(cfg, s, &v->impact_bp)) {
        v->pass = false;
        snprintf(v->reject, sizeof(v->reject), "ликвидность: арифметика");
        return 0;
    }
    uint32_t lmax = cfg->liquidity_max_bp ? cfg->liquidity_max_bp : 100;
    v->liquidity_ok = v->impact_bp <= lmax;
    if (!v->liquidity_ok) {
        v->pass = false;
        snprintf(v->reject, sizeof(v->reject),
                 "ликвидность: влияние %u б.п. > %u", v->impact_bp, lmax);
        return 0;
    }

    /* 3. Безопасность: dex в allowlist */
    if (cfg->dex_allow_n > 0) {
        v->safety_ok = false;
        for (uint32_t i = 0; i < cfg->dex_allow_n; i++)
            if (strcmp(cfg->dex_allow[i], s->dex) == 0) {
                v->safety_ok = true;
                break;
            }
        if (!v->safety_ok) {
            v->pass = false;
            snprintf(v->reject, sizeof(v->reject),
                     "безопасность: биржа '%s' вне allowlist", s->dex);
            return 0;
        }
    } else {
        v->safety_ok = true; /* allowlist не задан — не проверяем */
    }

    /* 4. Волатильность */
    if (!hist_vol_bp(h, &v->vol_bp)) {
        v->pass = false;
        snprintf(v->reject, sizeof(v->reject), "волатильность: арифметика");
        return 0;
    }
    uint32_t vmax = cfg->volatility_max_bp ? cfg->volatility_max_bp : 300;
    v->volatility_ok = v->vol_bp <= vmax;
    if (!v->volatility_ok) {
        v->pass = false;
        snprintf(v->reject, sizeof(v->reject),
                 "волатильность: %u б.п. > %u", v->vol_bp, vmax);
        return 0;
    }

    return 0;
}

void tb_arb_hist_push(tb_arb_hist_t *h, const tb_u256_t *buy_out)
{
    h->buf[h->pos] = *buy_out;
    h->pos = (h->pos + 1) % TB_ARB_HISTORY_LEN;
    if (h->n < TB_ARB_HISTORY_LEN)
        h->n++;
}
