#define TB_CURRENT_GROUP TB_G_ARB /* зона ответственности файла */
/* ==========================================================================
 *  tb/user/market.c — чистая сборка JSON-снапшота рынка (v0.5).
 *
 *  Вход: конфиг + массив снапшотов последнего батча (+ опциональная
 *  реальная котировка лучшей пары, В7). Выход: JSON для /api/market —
 *  без сети, без состояния: страница и юнит-тесты читают одно и то же.
 *
 *  Контракт пар (В1/В6): в таблицу спредов допускаются только полные
 *  снапшоты, свежие по now - ts <= freshness_max, с совпадающим блоком
 *  котировок (0 = блок неизвестен — пропускаем, оффлайн-режим).
 *  Профит в таблице — ОЦЕНКА probe × спред (в б.п.); реальная котировка
 *  лучшей пары — поле quote (В7).
 * ========================================================================== */
#include "tb/user/arb.h"

#include <stdio.h>
#include <string.h>

#define MARKET_PAIRS_MAX 15 /* верх таблицы: дальше — шум, не сигнал */

/* Безопасное добавление в буфер: snprintf возвращает желаемую длину —
 * слепое o += уходит за cap и портит стек; зажимаем и ставим NUL. */
#define MK_APPEND(...) do {                                            \
        if (o + 1 < cap) {                                             \
            int _n = snprintf(out + o, cap - o, __VA_ARGS__);          \
            if (_n > 0)                                                \
                o += (size_t)_n;                                       \
        }                                                              \
        if (o + 1 >= cap) {                                            \
            o = cap - 1;                                               \
            out[o] = '\0';                                           \
        }                                                              \
    } while (0)

/* Короткое имя цепи для страницы; неизвестная — её номер. */
static const char *chain_name(uint64_t chain)
{
    switch (chain) {
    case 1:      return "eth";
    case 8453:   return "base";
    case 42161:  return "arbitrum";
    case 0:      return "?";
    default: {
        static char buf[16];
        snprintf(buf, sizeof(buf), "%llu", (unsigned long long)chain);
        return buf;
    }
    }
}

int tb_arb_market_json(const tb_arb_cfg_t *cfg, const tb_arb_snap_t *snaps,
                       size_t n, uint64_t now_ms,
                       const tb_arb_route_quote_t *rq, char *out, size_t cap)
{
    if (!cfg || !out || cap == 0)
        return -1;
    if (!snaps)
        n = 0;
    if (n > TB_ARB_MAX_POOLS)
        n = TB_ARB_MAX_POOLS;

    uint64_t probe = cfg->probe_wei;
    uint32_t fmax = cfg->freshness_max_ms;
    if (!fmax)
        fmax = cfg->poll_ms ? 3 * (uint32_t)cfg->poll_ms : 24000;

    char probe_s[80];
    tb_u256_t pv = tb_u256_from_u64(probe);
    tb_u256_to_dec(&pv, probe_s, sizeof(probe_s));

    struct pair_ent {
        uint32_t bi, si;
        uint32_t bp;
        uint64_t prof;
    };
    struct pair_ent pairs[TB_ARB_MAX_POOLS * TB_ARB_MAX_POOLS];
    size_t pairs_n = 0;

    size_t o = 0;
    MK_APPEND("{\"probe_wei\":\"%s\",\"venues\":[", probe_s);

    char venues[TB_DEX_MAX_VENUES][TB_DEX_NAME_MAX];
    size_t venues_n = 0;
    for (size_t i = 0; i < n; i++) {
        size_t k = 0;
        for (; k < venues_n; k++)
            if (strcmp(venues[k], snaps[i].dex) == 0)
                break;
        if (k == venues_n && venues_n < TB_DEX_MAX_VENUES) {
            snprintf(venues[venues_n], sizeof(venues[0]), "%s", snaps[i].dex);
            venues_n++;
        }
    }

    for (size_t k = 0; k < venues_n; k++) {
        MK_APPEND("%s{\"name\":\"%s\",\"pools\":[",
                  k ? "," : "", venues[k]);
        int wrote = 0;
        for (size_t i = 0; i < n; i++) {
            if (strcmp(snaps[i].dex, venues[k]) != 0)
                continue;
            char pool_s[43], tok_s[43], buy_s[80];
            tb_addr_to_hex(&snaps[i].pool, pool_s, sizeof(pool_s));
            tb_addr_to_hex(&snaps[i].token, tok_s, sizeof(tok_s));
            tb_u256_to_dec(&snaps[i].buy_out, buy_s, sizeof(buy_s));
            uint32_t round_bp = 0;
            if (!tb_arb_ratio_bp(&snaps[i].sell_back, &pv, &round_bp))
                round_bp = 0;
            MK_APPEND("%s{\"pool\":\"%s\",\"token\":\"%s\","
                      "\"symbol\":\"%s\",\"decimals\":%u,"
                      "\"fee\":\"%u.%04u%%\",\"buy_out\":\"%s\","
                      "\"round_bp\":%u,\"chain\":\"%s\",\"block\":%llu,"
                      "\"ts_ms\":%llu,"
                      "\"fresh\":%s}",
                      wrote++ ? "," : "", pool_s, tok_s,
                      snaps[i].symbol[0] ? snaps[i].symbol : "",
                      snaps[i].decimals, snaps[i].fee / 10000,
                      snaps[i].fee % 10000, buy_s, round_bp,
                      chain_name(snaps[i].chain),
                      (unsigned long long)snaps[i].block,
                      (unsigned long long)snaps[i].ts_ms,
                      (now_ms - snaps[i].ts_ms <= fmax) ? "true" : "false");
        }
        MK_APPEND("]}");
    }

    for (size_t i = 0; i < n && pairs_n < TB_ARB_MAX_POOLS * TB_ARB_MAX_POOLS;
         i++) {
        if (!snaps[i].complete)
            continue;
        /* В1: устаревший снапшот не участвует в сравнении курсов */
        if (now_ms - snaps[i].ts_ms > fmax)
            continue;
        for (size_t j = 0; j < n; j++) {
            if (i == j || !snaps[j].complete)
                continue;
            /* В1: обе стороны пары обязаны быть свежими */
            if (now_ms - snaps[j].ts_ms > fmax)
                continue;
            if (!tb_addr_eq(&snaps[i].token, &snaps[j].token))
                continue;
            /* мультичейн: курсы сравнимы только в одной цепи (номер
             * блока и хаб чужой цепи не существуют) */
            if (snaps[i].chain != snaps[j].chain)
                continue;
            /* В6: разные блоки — не сравниваем (0 = неизвестен) */
            if (snaps[i].block != snaps[j].block)
                continue;
            uint32_t r = 0;
            if (!tb_arb_ratio_bp(&snaps[i].buy_out, &snaps[j].buy_out, &r)
                || r <= 10000)
                continue;
            struct pair_ent e = {
                .bi = (uint32_t)i,
                .si = (uint32_t)j,
                .bp = r - 10000,
                .prof = probe / 10000 * (uint64_t)(r - 10000),
            };
            size_t k = pairs_n++;
            for (; k > 0 && pairs[k - 1].bp < e.bp; k--)
                pairs[k] = pairs[k - 1];
            pairs[k] = e;
        }
    }

    MK_APPEND("],\"pairs\":[");
    for (size_t k = 0; k < pairs_n && k < MARKET_PAIRS_MAX; k++) {
        const tb_arb_snap_t *bs = &snaps[pairs[k].bi];
        const tb_arb_snap_t *ss = &snaps[pairs[k].si];
        char tok_s[43], prof_s[80];
        tb_addr_to_hex(&bs->token, tok_s, sizeof(tok_s));
        tb_u256_t pw = tb_u256_from_u64(pairs[k].prof);
        tb_u256_to_dec(&pw, prof_s, sizeof(prof_s));
        MK_APPEND("%s{\"token\":\"%s\",\"symbol\":\"%s\","
                  "\"buy\":\"%s %u.%04u%%\","
                  "\"sell\":\"%s %u.%04u%%\",\"spread_bp\":%u,"
                  "\"profit_est\":\"%s\"}",
                  k ? "," : "", tok_s, bs->symbol[0] ? bs->symbol : "",
                  bs->dex, bs->fee / 10000, bs->fee % 10000, ss->dex,
                  ss->fee / 10000, ss->fee % 10000, pairs[k].bp, prof_s);
    }

    /* В7: реальная котировка лучшей пары рядом с оценкой */
    if (rq && rq->ok) {
        char in_s[80], out_s[80], pf_s[80];
        tb_u256_to_dec(&rq->in_wei, in_s, sizeof(in_s));
        tb_u256_to_dec(&rq->out_wei, out_s, sizeof(out_s));
        tb_u256_to_dec(&rq->profit_wei, pf_s, sizeof(pf_s));
        MK_APPEND("],\"quote\":{\"symbol\":\"%s\",\"buy\":\"%s\","
                  "\"sell\":\"%s\",\"spread_bp\":%u,\"in_wei\":\"%s\","
                  "\"out_wei\":\"%s\",\"profit_wei\":\"%s%s\"}}",
                  rq->symbol, rq->buy_dex, rq->sell_dex, rq->spread_bp,
                  in_s, out_s, rq->profit_neg ? "-" : "", pf_s);
    } else {
        MK_APPEND("]}");
    }
    if (o < cap)
        out[o] = '\0';
    return 0;
}
