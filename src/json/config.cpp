#define TB_CURRENT_GROUP TB_G_CONFIG /* зона ответственности файла */
/* ==========================================================================
 *  src/json/config.cpp — загрузка конфигурации арбитража (JSON -> POD).
 *
 *  C++20/rapidjson (как Envelope) с чистым C-ABI: C-ядро не видит C++
 *  типов. Схема файла — docs/arb_config.example.json. Отсутствующие
 *  поля остаются нулями; дефолты применяет tb_arb_defaults (C).
 * ========================================================================== */
#include "tb/user/arb.h"

#include <cstdio>
#include <cstring>
#include <string>

#include "rapidjson/document.h"

static bool addr_field(const rapidjson::Value &v, const char *key,
                       tb_addr_t *out, tb_error_t *err)
{
    auto it = v.FindMember(key);
    if (it == v.MemberEnd())
        return true; /* поля нет — остаётся ноль */
    if (!it->value.IsString())
        return false;
    return tb_addr_from_hex(out, it->value.GetString(), err) == 0;
}

static bool u64_field(const rapidjson::Value &v, const char *key,
                      uint64_t *out)
{
    auto it = v.FindMember(key);
    if (it == v.MemberEnd())
        return true;
    if (it->value.IsUint64()) {
        *out = it->value.GetUint64();
        return true;
    }
    if (it->value.IsString()) { /* большие суммы — десятичной строкой */
        tb_u256_t tmp;
        tb_error_t e = TB_ERROR_INIT;
        if (tb_u256_from_dec(&tmp, it->value.GetString(),
                             it->value.GetStringLength(), &e) != 0)
            return false;
        if (tmp.w[1] || tmp.w[2] || tmp.w[3])
            return false; /* не влезает в u64 */
        *out = tmp.w[0];
        return true;
    }
    return false;
}

static bool str_field(const rapidjson::Value &v, const char *key, char *out,
                      size_t cap)
{
    auto it = v.FindMember(key);
    if (it == v.MemberEnd() || !it->value.IsString())
        return it == v.MemberEnd();
    snprintf(out, cap, "%s", it->value.GetString());
    return true;
}

static bool u32_field(const rapidjson::Value &v, const char *key,
                      uint32_t *out)
{
    uint64_t tmp = 0;
    if (!u64_field(v, key, &tmp) || tmp > 0xffffffffull)
        return false;
    if (tmp)
        *out = (uint32_t)tmp;
    return true;
}

extern "C" int tb_arb_cfg_load(tb_arb_cfg_t *out, const char *path,
                               tb_error_t *err)
{
    if (!out || !path)
        TB_ERROR_FAIL(err, TB_DOMAIN_CORE, TB_CORE_INVALID_ARG,
                      "config: аргументы");
    memset(out, 0, sizeof(*out));

    FILE *f = fopen(path, "rb");
    if (!f)
        TB_ERROR_FAIL(err, TB_DOMAIN_CORE, TB_CORE_IO, "config: нет '%s'",
                      path);
    std::string buf;
    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0)
        buf.append(chunk, n);
    fclose(f);

    rapidjson::Document doc;
    if (doc.Parse(buf.c_str(), buf.size()).HasParseError())
        TB_ERROR_FAIL(err, TB_DOMAIN_JSON, TB_JSON_PARSE,
                      "config: JSON-ошибка на смещении %zu",
                      (size_t)doc.GetErrorOffset());

    if (!doc.IsObject())
        TB_ERROR_FAIL(err, TB_DOMAIN_JSON, TB_JSON_SCHEMA,
                      "config: корень не объект");

    if (!addr_field(doc, "weth", &out->weth, err))
        TB_ERROR_FAIL(err, TB_DOMAIN_JSON, TB_JSON_SCHEMA,
                      "config: weth не адрес");
    if (tb_addr_is_native(&out->weth))
        TB_ERROR_FAIL(err, TB_DOMAIN_JSON, TB_JSON_SCHEMA,
                      "config: weth обязателен");

    /* rpc: [{name, url, quoter, router?, factory?, type?, host_pinned?}] —
     * пулы бирже тут НЕ объявляются (В9: реестр = pools[].dex ниже) */
    auto rpc = doc.FindMember("rpc");
    if (rpc != doc.MemberEnd() && rpc->value.IsArray()) {
        for (const auto &v : rpc->value.GetArray()) {
            if (out->dex.venues_n >= TB_DEX_MAX_VENUES)
                TB_ERROR_FAIL(err, TB_DOMAIN_JSON, TB_JSON_SCHEMA,
                              "config: много venues (макс %d)",
                              TB_DEX_MAX_VENUES);
            tb_exec_dex_venue_t &vn =
                out->dex.venues[out->dex.venues_n];
            if (!str_field(v, "name", vn.name, sizeof(vn.name))
                || !str_field(v, "url", vn.rpc_url, sizeof(vn.rpc_url))
                || !str_field(v, "host_pinned", vn.rpc_host_pinned,
                              sizeof(vn.rpc_host_pinned)))
                TB_ERROR_FAIL(err, TB_DOMAIN_JSON, TB_JSON_SCHEMA,
                              "config: rpc[%u] строки", out->dex.venues_n);
            if (!addr_field(v, "quoter", &vn.quoter, err))
                TB_ERROR_FAIL(err, TB_DOMAIN_JSON, TB_JSON_SCHEMA,
                              "config: rpc[%u].quoter не адрес",
                              out->dex.venues_n);
            if (!addr_field(v, "router", &vn.router, err))
                TB_ERROR_FAIL(err, TB_DOMAIN_JSON, TB_JSON_SCHEMA,
                              "config: rpc[%u].router не адрес",
                              out->dex.venues_n);
            if (!addr_field(v, "factory", &vn.factory, err))
                TB_ERROR_FAIL(err, TB_DOMAIN_JSON, TB_JSON_SCHEMA,
                              "config: rpc[%u].factory не адрес",
                              out->dex.venues_n);
            { /* тип биржи: метод котировки хопа; нет поля -> V3 */
                auto t = v.FindMember("type");
                if (t != v.MemberEnd()) {
                    if (!t->value.IsString())
                        TB_ERROR_FAIL(err, TB_DOMAIN_JSON, TB_JSON_SCHEMA,
                                      "config: rpc[%u].type не строка",
                                      out->dex.venues_n);
                    const char *s = t->value.GetString();
                    if (strcmp(s, "v3") == 0)
                        vn.kind = TB_DEX_V3;
                    else if (strcmp(s, "v2") == 0)
                        vn.kind = TB_DEX_V2;
                    else
                        TB_ERROR_FAIL(err, TB_DOMAIN_JSON, TB_JSON_SCHEMA,
                                      "config: rpc[%u].type '%s' (v2|v3)",
                                      out->dex.venues_n, s);
                }
            }
            { /* мультичейн: цепь и хаб-токен биржи; нет -> общий */
                uint64_t ch = 0;
                if (!u64_field(v, "chain_id", &ch))
                    TB_ERROR_FAIL(err, TB_DOMAIN_JSON, TB_JSON_SCHEMA,
                                  "config: rpc[%u].chain_id",
                                  out->dex.venues_n);
                vn.chain_id = ch;
                if (!addr_field(v, "weth", &vn.weth, err))
                    TB_ERROR_FAIL(err, TB_DOMAIN_JSON, TB_JSON_SCHEMA,
                                  "config: rpc[%u].weth не адрес",
                                  out->dex.venues_n);
            }
            uint64_t tmo = 0;
            if (!u64_field(v, "timeout_ms", &tmo))
                TB_ERROR_FAIL(err, TB_DOMAIN_JSON, TB_JSON_SCHEMA,
                              "config: rpc[%u].timeout_ms",
                              out->dex.venues_n);
            vn.timeout_ms = (uint32_t)tmo;
            out->dex.venues_n++;
        }
    }

    /* pools: [{token, pool, fee, dex}] */
    auto pools = doc.FindMember("pools");
    if (pools != doc.MemberEnd() && pools->value.IsArray()) {
        for (const auto &v : pools->value.GetArray()) {
            if (out->pools_n >= TB_ARB_MAX_POOLS)
                TB_ERROR_FAIL(err, TB_DOMAIN_JSON, TB_JSON_SCHEMA,
                              "config: много пулов (макс %d)",
                              TB_ARB_MAX_POOLS);
            tb_arb_pool_t &p = out->pools[out->pools_n];
            if (!addr_field(v, "token", &p.token, err)
                || !addr_field(v, "pool", &p.pool, err))
                TB_ERROR_FAIL(err, TB_DOMAIN_JSON, TB_JSON_SCHEMA,
                              "config: pools[%u] адреса", out->pools_n);
            if (!u32_field(v, "fee", &p.fee))
                TB_ERROR_FAIL(err, TB_DOMAIN_JSON, TB_JSON_SCHEMA,
                              "config: pools[%u].fee", out->pools_n);
            if (!str_field(v, "dex", p.dex, sizeof(p.dex)))
                TB_ERROR_FAIL(err, TB_DOMAIN_JSON, TB_JSON_SCHEMA,
                              "config: pools[%u].dex", out->pools_n);
            out->pools_n++;
        }
    }

    /* В9 — реестр: пул приписывается бирже ИМЕННО по pools[].dex, здесь
     * и в одном месте. Двойное объявление (rpc[].pools + pools[]) убрано:
     * рассинхрон невозможен по построению. */
    for (uint32_t i = 0; i < out->pools_n; i++) {
        tb_arb_pool_t &p = out->pools[i];
        tb_exec_dex_venue_t *venue =
            (tb_exec_dex_venue_t *)tb_arb_venue_by_name(out, p.dex);
        if (!venue)
            TB_ERROR_FAIL(err, TB_DOMAIN_JSON, TB_JSON_SCHEMA,
                          "config: pools[%u].dex '%s' нет в rpc[]", i,
                          p.dex);
        if (venue->pools_n >= TB_DEX_POOLS_PER_VENUE)
            TB_ERROR_FAIL(err, TB_DOMAIN_JSON, TB_JSON_SCHEMA,
                          "config: у биржи %s максимум пулов (%d)", p.dex,
                          TB_DEX_POOLS_PER_VENUE);
        venue->pools[venue->pools_n++] = p.pool;
        /* мультичейн: пул несёт цепь и хаб своей биржи; дефолты venue —
         * после network-секции (chain_id известен только там) */
        p.chain = venue->chain_id;
        p.weth = venue->weth;
    }

    /* filters: {freshness_ms, impact_bp, volatility_bp, probe_mult,
     *           dex_allow: [names]} */
    auto flt = doc.FindMember("filters");
    if (flt != doc.MemberEnd() && flt->value.IsObject()) {
        const auto &v = flt->value;
        if (!u32_field(v, "freshness_ms", &out->freshness_max_ms)
            || !u32_field(v, "impact_bp", &out->liquidity_max_bp)
            || !u32_field(v, "volatility_bp", &out->volatility_max_bp)
            || !u32_field(v, "probe_mult", &out->liquidity_probe_mult))
            TB_ERROR_FAIL(err, TB_DOMAIN_JSON, TB_JSON_SCHEMA,
                          "config: filters числа");
        auto allow = v.FindMember("dex_allow");
        if (allow != v.MemberEnd() && allow->value.IsArray()) {
            for (const auto &av : allow->value.GetArray()) {
                if (!av.IsString())
                    TB_ERROR_FAIL(err, TB_DOMAIN_JSON, TB_JSON_SCHEMA,
                                  "config: dex_allow[] не строки");
                if (out->dex_allow_n >= TB_DEX_MAX_VENUES)
                    TB_ERROR_FAIL(err, TB_DOMAIN_JSON, TB_JSON_SCHEMA,
                                  "config: dex_allow длинный");
                snprintf(out->dex_allow[out->dex_allow_n++],
                         TB_DEX_NAME_MAX, "%s", av.GetString());
            }
        }
    }

    /* decision: {poll_ms, probe_wei, min/max_trade_wei, min_profit_wei,
     *            gas_est_wei, deadline_ms, max_pairs} */
    auto dec = doc.FindMember("decision");
    if (dec != doc.MemberEnd() && dec->value.IsObject()) {
        const auto &v = dec->value;
        if (!u64_field(v, "poll_ms", &out->poll_ms)
            || !u64_field(v, "probe_wei", &out->probe_wei)
            || !u64_field(v, "min_trade_wei", &out->min_trade_wei)
            || !u64_field(v, "max_trade_wei", &out->max_trade_wei)
            || !u64_field(v, "min_profit_wei", &out->min_profit_wei)
            || !u64_field(v, "gas_est_wei", &out->gas_est_wei)
            || !u64_field(v, "deadline_ms", &out->deadline_ms)
            || !u32_field(v, "max_pairs", &out->max_pairs_per_poll))
            TB_ERROR_FAIL(err, TB_DOMAIN_JSON, TB_JSON_SCHEMA,
                          "config: decision числа");
    }

    /* network: {chain_id, safe, router, key_file, live_exec, max_fee_wei} */
    auto net = doc.FindMember("network");
    if (net != doc.MemberEnd() && net->value.IsObject()) {
        const auto &v = net->value;
        uint64_t chain = 0, max_fee = 0;
        if (!u64_field(v, "chain_id", &chain)
            || !u64_field(v, "max_fee_wei", &max_fee))
            TB_ERROR_FAIL(err, TB_DOMAIN_JSON, TB_JSON_SCHEMA,
                          "config: network числа");
        out->dex.live = false;
        auto le = v.FindMember("live_exec");
        if (le != v.MemberEnd() && le->value.IsBool())
            out->dex.live = le->value.GetBool();
        out->dex.safe.chain_id = chain;
        out->dex.safe.max_fee_wei = max_fee;
        if (!addr_field(v, "safe", &out->dex.safe.safe, err))
            TB_ERROR_FAIL(err, TB_DOMAIN_JSON, TB_JSON_SCHEMA,
                          "config: network.safe не адрес");
        if (!addr_field(v, "router", &out->dex.safe.router, err))
            TB_ERROR_FAIL(err, TB_DOMAIN_JSON, TB_JSON_SCHEMA,
                          "config: network.router не адрес");
        if (!str_field(v, "key_file", out->key_file,
                       sizeof(out->key_file)))
            TB_ERROR_FAIL(err, TB_DOMAIN_JSON, TB_JSON_SCHEMA,
                          "config: network.key_file");
    }

    /* fill_delay для shadow-бэкенда */
    uint64_t fmin = 0, fmax = 0;
    if (dec != doc.MemberEnd() && dec->value.IsObject()) {
        if (!u64_field(dec->value, "fill_delay_min_ms", &fmin)
            || !u64_field(dec->value, "fill_delay_max_ms", &fmax))
            TB_ERROR_FAIL(err, TB_DOMAIN_JSON, TB_JSON_SCHEMA,
                          "config: fill_delay");
    }
    out->dex.fill_delay_min_ms = (uint32_t)fmin;
    out->dex.fill_delay_max_ms = (uint32_t)fmax;

    /* мультичейн-дефолты (после network): цепь 0 -> network.chain_id
     * (сам -> 1), хаб 0 -> общий weth конфига; пулы догоняют биржи */
    {
        uint64_t dflt_chain =
            out->dex.safe.chain_id ? out->dex.safe.chain_id : 1;
        for (uint32_t v = 0; v < out->dex.venues_n; v++) {
            if (!out->dex.venues[v].chain_id)
                out->dex.venues[v].chain_id = dflt_chain;
            if (tb_addr_is_native(&out->dex.venues[v].weth))
                out->dex.venues[v].weth = out->weth;
        }
        for (uint32_t i = 0; i < out->pools_n; i++) {
            if (!out->pools[i].chain
                || tb_addr_is_native(&out->pools[i].weth)) {
                const tb_exec_dex_venue_t *venue =
                    tb_arb_venue_by_name(out, out->pools[i].dex);
                if (venue) {
                    if (!out->pools[i].chain)
                        out->pools[i].chain = venue->chain_id;
                    if (tb_addr_is_native(&out->pools[i].weth))
                        out->pools[i].weth = venue->weth;
                }
            }
        }
    }

    /* web: {port, bind, refresh_ms, pages[]} — какие страницы-микромодули
     * подняты; bind — listen-адрес ("0.0.0.0" = с других машин) */
    auto web = doc.FindMember("web");
    if (web != doc.MemberEnd() && web->value.IsObject()) {
        const auto &v = web->value;
        uint64_t port = 0, refr = 0;
        if (!u64_field(v, "port", &port) || !u64_field(v, "refresh_ms", &refr))
            TB_ERROR_FAIL(err, TB_DOMAIN_JSON, TB_JSON_SCHEMA,
                          "config: web числа");
        out->web.port = (uint16_t)port;
        out->web.refresh_ms = (uint32_t)refr;
        if (!str_field(v, "bind", out->web.bind, sizeof(out->web.bind)))
            TB_ERROR_FAIL(err, TB_DOMAIN_JSON, TB_JSON_SCHEMA,
                          "config: web.bind");
        auto ps = v.FindMember("pages");
        if (ps != v.MemberEnd() && ps->value.IsArray()) {
            for (auto &p : ps->value.GetArray()) {
                if (!p.IsString() || out->web.pages_n >= TB_ARB_WEB_PAGES_MAX)
                    continue;
                snprintf(out->web.pages[out->web.pages_n],
                         sizeof(out->web.pages[0]), "%s", p.GetString());
                out->web.pages_n++;
            }
        }
    }

    return 0;
}

extern "C" void tb_arb_defaults(tb_arb_cfg_t *cfg)
{
    if (!cfg)
        return;
    if (cfg->freshness_max_ms == 0)
        cfg->freshness_max_ms = cfg->poll_ms ? 3 * (uint32_t)cfg->poll_ms
                                             : 24000;
    if (cfg->liquidity_probe_mult == 0)
        cfg->liquidity_probe_mult = 10;
    if (cfg->liquidity_max_bp == 0)
        cfg->liquidity_max_bp = 100;
    if (cfg->volatility_max_bp == 0)
        cfg->volatility_max_bp = 300;
    if (cfg->poll_ms == 0)
        cfg->poll_ms = 8000;
    if (cfg->probe_wei == 0)
        cfg->probe_wei = 100000000000000ull;
    if (cfg->min_trade_wei == 0)
        cfg->min_trade_wei = 100000000000000ull;
    if (cfg->max_trade_wei == 0)
        cfg->max_trade_wei = 10000000000000000ull;
    if (cfg->min_profit_wei == 0)
        cfg->min_profit_wei = 10000000000000ull;
    if (cfg->gas_est_wei == 0)
        cfg->gas_est_wei = 200000000000000ull;
    if (cfg->deadline_ms == 0)
        cfg->deadline_ms = 8000;
    if (cfg->max_pairs_per_poll == 0)
        cfg->max_pairs_per_poll = 1;

    /* веб: дефолты страницы; 0 -> все стандартные */
    if (cfg->web.port == 0)
        cfg->web.port = 8080;
    if (cfg->web.refresh_ms == 0)
        cfg->web.refresh_ms = 2000;
    if (cfg->web.bind[0] == '\0')
        snprintf(cfg->web.bind, sizeof(cfg->web.bind), "127.0.0.1");
    if (cfg->web.pages_n == 0) {
        const char *std[] = {"dashboard", "venues", "trades"};
        for (const char *s : std)
            snprintf(cfg->web.pages[cfg->web.pages_n++],
                     sizeof(cfg->web.pages[0]), "%s", s);
    }
}
