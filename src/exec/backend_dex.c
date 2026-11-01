/* ==========================================================================
 *  tb/exec/backend_dex.c — живой DEX-адаптер: QuoterV2 через eth_call.
 *
 *  Запрос (хоп): quoteExactInputSingle((tokenIn,tokenOut,amountIn,fee,0)),
 *  селектор 0xc6a5026a, ответ — 4 слова, слово 0 = amountOut. Маршрут
 *  котируется последовательно: выход хопа i — вход хопа i+1 (комиссии и
 *  влияние уже в числах Quoter — дискретная математика без магии).
 *
 *  Разбор ответа — минимальный сканер форм JSON-RPC (result/error),
 *  библиотека не нужна: формы машинные, hex-значения без экранирования.
 *
 *  Ошибки RPC: transport/429/5xx -> TB_EXEC_BACKEND (retryable),
 *  execution reverted / пустой result -> TB_EXEC_INSUFFICIENT_LIQUIDITY
 *  (пула нет или он пуст — повтор бессмыслен до движения рынка).
 * ========================================================================== */
#include "tb/exec/backend_dex.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tb/arch/arch.h"
#include "tb/core/time.h"

/* ---- Состояние ---- */

typedef struct dex_session {
    uint64_t   tx_id;      /* != 0 — занято */
    tb_u256_t  amount_out; /* цена submit-котировки: поллинг цену не меняет */
    uint64_t   fill_at_ms;
} dex_session_t;

typedef struct dex_ctx {
    tb_exec_dex_cfg_t cfg;
    tb_http_t        *http;
    uint64_t          rand_state;
    uint64_t          next_tx;
    dex_session_t     txs[TB_DEX_MAX_SESSIONS];
    tb_exec_dex_stats_t stats;
} dex_ctx_t;

/* ---- Мини-сканер JSON-RPC ответов ---- */

/* Значение строкового поля "key" (без кавычек). 0 — не найдено. */
static const char *json_str_field(const char *body, const char *key,
                                  size_t *len_out)
{
    char pat[32];
    int n = snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = body;
    while ((p = strstr(p, pat)) != NULL) {
        if (p > body && p[-1] == '\\') { /* экранированное вхождение */
            p += (size_t)n;
            continue;
        }
        p += (size_t)n;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p != ':')
            continue;
        p++;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p != '"')
            continue;
        p++;
        const char *e = p;
        while (*e && *e != '"') {
            if (*e == '\\' && e[1])
                e++;
            e++;
        }
        if (*e != '"')
            return NULL;
        *len_out = (size_t)(e - p);
        return p;
    }
    return NULL;
}

/* Есть ли поле error (не экранированное). */
static bool json_has_error(const char *body)
{
    const char *p = body;
    while ((p = strstr(p, "\"error\"")) != NULL) {
        if (p == body || p[-1] != '\\')
            return true;
        p += 7;
    }
    return false;
}

/* ---- hex <-> u256 (256-бит, big-endian по байтам EVM) ---- */

static size_t hex_val(char c)
{
    if (c >= '0' && c <= '9')
        return (size_t)(c - '0');
    if (c >= 'a' && c <= 'f')
        return (size_t)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F')
        return (size_t)(c - 'A' + 10);
    return (size_t)-1;
}

static int hex_to_u256(tb_u256_t *out, const char *hex, size_t len,
                       tb_error_t *err)
{
    if (len < 2 || len > 66 || hex[0] != '0' || hex[1] != 'x')
        goto bad;
    hex += 2;
    len -= 2;
    if (len == 0) { /* "0x" — пустой ответ: функции/пула нет */
        *out = tb_u256_from_u64(0);
        return 0;
    }

    *out = tb_u256_from_u64(0);
    for (size_t i = 0; i < len; i++) {
        size_t v = hex_val(hex[i]);
        if (v == (size_t)-1)
            goto bad;
        /* out = out*16 + v */
        if (!tb_u256_mul_u64(out, out, 16))
            goto bad;
        tb_u256_t add = tb_u256_from_u64(v);
        if (!tb_u256_add(out, out, &add))
            goto bad;
    }
    return 0;
bad:
    TB_ERROR_SET(err, TB_DOMAIN_USER, TB_EXEC_BACKEND,
                 "RPC: hex-значение некорректно");
    return -1;
}

static size_t u256_to_hex(const tb_u256_t *v, char *buf, size_t cap)
{
    uint8_t bytes[32];
    for (int i = 0; i < 4; i++)
        for (int b = 0; b < 8; b++)
            bytes[31 - (i * 8 + b)] =
                (uint8_t)(v->w[i] >> (8 * b)); /* слова LE -> байты BE */

    int start = 0;
    while (start < 31 && bytes[start] == 0)
        start++;

    size_t n = 0;
    buf[n++] = '0';
    buf[n++] = 'x';
    for (int i = start; i < 32; i++) {
        if (n + 3 >= cap)
            break;
        n += (size_t)snprintf(buf + n, 3, "%02x", bytes[i]);
    }
    if (n == 2 && n + 2 < cap) {
        buf[n++] = '0'; /* ноль как "0x0" */
    }
    buf[n] = '\0';
    return n;
}

/* 32-байтовое ABI-слово из hex-значения без "0x": ведущие нули до 64
 * символов. hex_value_len <= 64. */
static void hex_word64(const char *hex_value, size_t len, char out[65])
{
    size_t digits = len > 64 ? 64 : len;
    memset(out, '0', 64 - digits);
    memcpy(out + (64 - digits), hex_value, digits);
    out[64] = '\0';
}

/* ---- eth_call к QuoterV2: один хоп ---- */

static const tb_exec_dex_venue_t *venue_by_pool(const dex_ctx_t *c,
                                                const tb_addr_t *pool)
{
    if (tb_addr_is_native(pool))
        return NULL;
    for (uint32_t v = 0; v < c->cfg.venues_n; v++)
        for (uint32_t p = 0; p < c->cfg.venues[v].pools_n; p++)
            if (tb_addr_eq(&c->cfg.venues[v].pools[p], pool))
                return &c->cfg.venues[v];
    return NULL;
}

/* Котировка одного хопа: out = quoter(tokenIn -> tokenOut, fee, in).
 * 0 — успех. */
static int dex_quote_hop(dex_ctx_t *c, const tb_exec_dex_venue_t *venue,
                         const tb_addr_t *tin, const tb_addr_t *tout,
                         uint32_t fee, const tb_u256_t *amount_in,
                         tb_u256_t *out, tb_error_t *err)
{
    char qhex[43], inhex[43], ahex[80];
    tb_addr_to_hex(&venue->quoter, qhex, sizeof(qhex));
    tb_addr_to_hex(tin, inhex, sizeof(inhex));
    tb_addr_to_hex(tout, ahex, sizeof(ahex)); /* out-токен */
    char amt[80];
    u256_to_hex(amount_in, amt, sizeof(amt));

    /* calldata: selector + tokenIn + tokenOut + amountIn + fee + 0 —
     * каждое поле выровнено в 32-байтовое слово нулями слева. */
    char w_in[65], w_out[65], w_amt[65], w_fee[65], w_zero[65];
    hex_word64(inhex + 2, 40, w_in);
    hex_word64(ahex + 2, 40, w_out);
    hex_word64(amt + 2, strlen(amt) - 2, w_amt);
    {
        char feehex[17];
        snprintf(feehex, sizeof(feehex), "%x", fee);
        hex_word64(feehex, strlen(feehex), w_fee);
    }
    hex_word64("", 0, w_zero);

    char data[8 + 64 * 5 + 8]; /* "0x" + селектор + 5 слов */
    snprintf(data, sizeof(data), "0x%08x%s%s%s%s%s", 0xc6a5026au,
             w_in, w_out, w_amt, w_fee, w_zero);

    char body[640];
    int n = snprintf(body, sizeof(body),
                     "{\"jsonrpc\":\"2.0\",\"method\":\"eth_call\","
                     "\"params\":[{\"to\":\"%s\",\"data\":\"%s\"},"
                     "\"latest\"],\"id\":1}",
                     qhex, data);
    if (n < 0 || (size_t)n >= sizeof(body))
        TB_ERROR_FAIL(err, TB_DOMAIN_USER, TB_EXEC_BACKEND,
                      "RPC-запрос не влез в буфер");

    tb_http_req_t req = {0};
    strcpy(req.method, "POST");
    strcpy(req.url, venue->rpc_url);
    if (venue->rpc_host_pinned[0])
        strcpy(req.host_pinned, venue->rpc_host_pinned);
    req.body = body;
    req.body_len = (size_t)n;
    req.timeout_ms = venue->timeout_ms ? venue->timeout_ms : 8000;

    if (getenv("TB_DEX_DEBUG"))
        fprintf(stderr, "[dex] REQ body: %s\n", body);
    tb_http_resp_t *resp = calloc(1, sizeof(*resp));
    if (!resp)
        TB_ERROR_FAIL(err, TB_DOMAIN_CORE, TB_CORE_NO_MEMORY, "dex: resp");
    int rc = -1;
    c->stats.rpc_calls++;

    if (tb_http_request(c->http, &req, resp, err) != 0) {
        c->stats.rpc_errors++;
        char reason[96];
        snprintf(reason, sizeof(reason), "%s", err ? err->message : "?");
        TB_ERROR_SET(err, TB_DOMAIN_USER, TB_EXEC_BACKEND,
                     "RPC %s: %s", venue->name, reason);
        err->retryable = true; /* транспортные сбои повторяемы */
        goto out;
    }
    if (resp->status == 429 || resp->status >= 500) {
        c->stats.rpc_errors++;
        tb_error_t *te = err;
        TB_ERROR_SET(te, TB_DOMAIN_USER, TB_EXEC_BACKEND,
                     "RPC %s: HTTP %ld%s", venue->name, (long)resp->status,
                     resp->retry_after_ms ? " (Retry-After)" : "");
        te->retryable = true;
        te->retry_after_ms = resp->retry_after_ms;
        goto out;
    }
    if (resp->status != 200) {
        c->stats.rpc_errors++;
        TB_ERROR_SET(err, TB_DOMAIN_USER, TB_EXEC_BACKEND,
                     "RPC %s: неожиданный HTTP %ld", venue->name,
                     (long)resp->status);
        goto out;
    }

    resp->body[resp->body_len < TB_HTTP_BODY_MAX ? resp->body_len
                                                 : TB_HTTP_BODY_MAX - 1] = '\0';

    size_t rlen = 0;
    if (getenv("TB_DEX_DEBUG"))
        fprintf(stderr, "[dex] HTTP %ld body: %.400s\n", (long)resp->status,
                resp->body);
    const char *res = json_str_field(resp->body, "result", &rlen);
    if (!res || json_has_error(resp->body)) {
        c->stats.rpc_errors++;
        size_t mlen = 0;
        const char *msg = json_str_field(resp->body, "message", &mlen);
        char mbuf[160] = "нет сообщения";
        if (msg && mlen < sizeof(mbuf)) {
            memcpy(mbuf, msg, mlen);
            mbuf[mlen] = '\0';
        }
        /* execution reverted у Quoter — пул не существует или пуст */
        bool reverted = strstr(mbuf, "revert") != NULL;
        TB_ERROR_SET(err, TB_DOMAIN_USER,
                     reverted ? TB_EXEC_INSUFFICIENT_LIQUIDITY
                              : TB_EXEC_BACKEND,
                     "RPC %s: %s", venue->name, mbuf);
        goto out;
    }
    /* ответ QuoterV2 — 4 слова; слово 0 (первые 32 байта) = amountOut */
    if (getenv("TB_DEX_DEBUG"))
        fprintf(stderr, "[dex] result hex (rlen=%zu): %.134s\n", rlen, res);
    if (hex_to_u256(out, res, rlen > 66 ? 66 : rlen, err) != 0) {
        c->stats.rpc_errors++;
        tb_error_wrap(err, TB_LOC, "RPC %s", venue->name);
        goto out;
    }
    if (tb_u256_is_zero(out)) {
        c->stats.rpc_errors++;
        TB_ERROR_SET(err, TB_DOMAIN_USER, TB_EXEC_INSUFFICIENT_LIQUIDITY,
                     "RPC %s: нулевая котировка (пул пуст?)", venue->name);
        goto out;
    }
    rc = 0;

out:
    free(resp);
    return rc;
}

/* Полная котировка маршрута по хопам (пулы из route). */
static int dex_quote_route(dex_ctx_t *c, const tb_route_t *route,
                           const tb_u256_t *amount_in, tb_u256_t *out,
                           tb_error_t *err)
{
    size_t hops = tb_route_hops(route);
    if (hops == 0) {
        TB_ERROR_SET(err, TB_DOMAIN_USER, TB_EXEC_INVALID_PATH,
                     "dex: маршрут пуст");
        return -1;
    }
    tb_u256_t v = *amount_in;
    for (size_t h = 0; h < hops; h++) {
        tb_addr_t pool = tb_route_pool(route, h);
        const tb_exec_dex_venue_t *venue = venue_by_pool(c, &pool);
        if (!venue) {
            TB_ERROR_SET(err, TB_DOMAIN_USER, TB_EXEC_BACKEND,
                         "dex: пул хопа %zu не приписан бирже", h);
            return -1;
        }
        tb_addr_t tin = tb_route_token(route, h);
        tb_addr_t tout = tb_route_token(route, h + 1);
        if (dex_quote_hop(c, venue, &tin, &tout, tb_route_fee(route, h),
                          &v, out, err) != 0) {
            tb_error_wrap(err, TB_LOC, "хоп %zu (%s)", h, venue->name);
            return -1;
        }
        v = *out;
    }
    c->stats.quotes++;
    return 0;
}

/* ---- vtable ---- */

static void dex_destroy(tb_exec_backend_t *b)
{
    free(b->ctx);
    b->ctx = NULL;
}

static int dex_quote(tb_exec_backend_t *b, const tb_route_t *route,
                     const tb_u256_t *amount_in, uint64_t now_ms,
                     tb_u256_t *out, tb_error_t *err)
{
    (void)now_ms;
    return dex_quote_route(b->ctx, route, amount_in, out, err);
}

static uint64_t dex_rand(dex_ctx_t *c)
{
    c->rand_state ^= c->rand_state << 13;
    c->rand_state ^= c->rand_state >> 7;
    c->rand_state ^= c->rand_state << 17;
    return c->rand_state;
}

static int dex_submit(tb_exec_backend_t *b, const tb_route_t *route,
                      const tb_u256_t *amount_in, const tb_u256_t *min_out,
                      uint64_t deadline_ms, uint64_t now_ms,
                      uint64_t *tx_id, tb_u256_t *amount_out, tb_error_t *err)
{
    (void)deadline_ms;
    dex_ctx_t *c = b->ctx;
    c->stats.submits++;

    /* Свежая котировка на момент отправки (как живой роутер). */
    tb_u256_t out;
    if (dex_quote_route(c, route, amount_in, &out, err) != 0)
        return -1;

    if (min_out && tb_u256_cmp(&out, min_out) < 0) {
        TB_ERROR_SET(err, TB_DOMAIN_USER, TB_EXEC_SLIPPAGE,
                     "dex: котировка ниже минимума ордера (роутер бы "
                     "ревертил)");
        return -1;
    }

    dex_session_t *s = NULL;
    for (size_t i = 0; i < TB_DEX_MAX_SESSIONS; i++)
        if (c->txs[i].tx_id == 0) {
            s = &c->txs[i];
            break;
        }
    if (!s)
        TB_ERROR_FAIL(err, TB_DOMAIN_USER, TB_EXEC_CAPACITY,
                      "dex: таблица сессий полна");

    uint32_t dmin = c->cfg.fill_delay_min_ms ? c->cfg.fill_delay_min_ms : 300;
    uint32_t dmax = c->cfg.fill_delay_max_ms ? c->cfg.fill_delay_max_ms : 900;
    if (dmax < dmin)
        dmax = dmin;

    s->tx_id = ++c->next_tx;
    s->amount_out = out;
    s->fill_at_ms = now_ms + dmin + dex_rand(c) % (dmax - dmin + 1);

    *tx_id = s->tx_id;
    *amount_out = out;
    return 1; /* SUBMITTED: поллинг заполнит по цене котировки */
}

static int dex_poll(tb_exec_backend_t *b, uint64_t tx_id, uint64_t now_ms,
                    tb_u256_t *amount_out, tb_error_t *err)
{
    dex_ctx_t *c = b->ctx;
    dex_session_t *s = NULL;
    for (size_t i = 0; i < TB_DEX_MAX_SESSIONS; i++)
        if (c->txs[i].tx_id == tx_id) {
            s = &c->txs[i];
            break;
        }
    if (!s)
        TB_ERROR_FAIL(err, TB_DOMAIN_USER, TB_EXEC_BACKEND,
                      "dex: неизвестная сессия %llu",
                      (unsigned long long)tx_id);

    if (now_ms < s->fill_at_ms)
        return 1; /* ещё в работе */

    *amount_out = s->amount_out;
    s->tx_id = 0;
    c->stats.fills++;
    return 0; /* заполнено (shadow: по цене submit-котировки) */
}

/* ---- init ---- */

int tb_exec_dex_init(tb_exec_backend_t *b, tb_http_t *http,
                     const tb_exec_dex_cfg_t *cfg, tb_error_t *err)
{
    if (!b || !http || !cfg)
        TB_ERROR_FAIL(err, TB_DOMAIN_CORE, TB_CORE_INVALID_ARG,
                      "dex: аргументы");
    if (cfg->venues_n == 0 || cfg->venues_n > TB_DEX_MAX_VENUES)
        TB_ERROR_FAIL(err, TB_DOMAIN_CORE, TB_CORE_INVALID_ARG,
                      "dex: venues_n вне диапазона");

    memset(b, 0, sizeof(*b));
    dex_ctx_t *c = calloc(1, sizeof(*c));
    if (!c)
        TB_ERROR_FAIL(err, TB_DOMAIN_CORE, TB_CORE_NO_MEMORY, "dex: ctx");
    c->cfg = *cfg;
    c->http = http;
    c->rand_state = cfg->seed ? cfg->seed : tb_rdtsc();

    for (uint32_t v = 0; v < c->cfg.venues_n; v++) {
        if (!c->cfg.venues[v].rpc_url[0] || c->cfg.venues[v].pools_n == 0) {
            free(c);
            TB_ERROR_FAIL(err, TB_DOMAIN_CORE, TB_CORE_INVALID_ARG,
                          "dex: venue %u без rpc_url или пулов", v);
        }
    }

    b->name = "dex-rpc";
    b->ctx = c;
    b->destroy = dex_destroy;
    b->quote = dex_quote;
    b->submit = dex_submit;
    b->poll = dex_poll;
    return 0;
}

const tb_exec_dex_stats_t *tb_exec_dex_stats(const tb_exec_backend_t *b)
{
    if (!b || !b->ctx)
        return NULL;
    return &((dex_ctx_t *)b->ctx)->stats;
}
