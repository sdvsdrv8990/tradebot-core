#define TB_CURRENT_GROUP TB_G_DEX /* зона ответственности файла */
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
#include "tb/core/detail.h"
#include "tb/core/keccak.h"
#include "tb/core/time.h"

/* ---- Состояние ---- */

typedef struct dex_session {
    uint64_t   tx_id;      /* != 0 — занято */
    tb_u256_t  amount_out; /* shadow: цена котировки; live: оценка,
                            * истина — дельта баланса после исполнения */
    uint64_t   fill_at_ms; /* shadow: момент заполнения */
    /* live (P0): предложение SafeTx ждёт подписей человека */
    bool       live;
    uint64_t   safe_nonce; /* nonce Safe на момент предложения */
    uint64_t   expires_ms; /* окно наблюдения; истекло — отказ без цепи */
    tb_addr_t  out_token;  /* последний токен маршрута */
    tb_u256_t  balance_before; /* для дельты после роста nonce */
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

/* ---- Пин блока котировок (TLS): батч пинится своим блоком (В6) ---- */

static _Thread_local uint64_t t_quote_block; /* 0 -> "latest" */

void tb_exec_dex_set_quote_block(uint64_t block)
{
    t_quote_block = block;
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

/* Котировка одного хопа: out = venue-метод(tokenIn -> tokenOut, fee, in).
 * V3 — QuoterV2.quoteExactInputSingle, V2 — Router.getAmountsOut.
 * 0 — успех. */
static int dex_quote_hop(dex_ctx_t *c, const tb_exec_dex_venue_t *venue,
                         const tb_addr_t *tin, const tb_addr_t *tout,
                         uint32_t fee, const tb_u256_t *amount_in,
                         tb_u256_t *out, tb_error_t *err)
{
    char inhex[43], ahex[80];
    tb_addr_to_hex(tin, inhex, sizeof(inhex));
    tb_addr_to_hex(tout, ahex, sizeof(ahex)); /* out-токен */
    char amt[80];
    u256_to_hex(amount_in, amt, sizeof(amt));

    const bool is_v2 = venue->kind == TB_DEX_V2;
    char qhex[43];
    tb_addr_to_hex(is_v2 ? &venue->router : &venue->quoter, qhex,
                   sizeof(qhex));

    char data[8 + 64 * 5 + 8]; /* "0x" + селектор + до 5 слов (V3) */
    if (is_v2) {
        /* getAmountsOut(uint256, address[]): сумма, смещение 0x40,
         * длина пути 2, токены; комиссия 0.30% — внутри котировки */
        if (tb_addr_is_native(&venue->router))
            TB_ERROR_FAIL(err, TB_DOMAIN_USER, TB_EXEC_BACKEND,
                          "dex: venue %s (V2) без router", venue->name);
        uint8_t sel[4];
        tb_keccak_selector("getAmountsOut(uint256,address[])", sel);
        char w_amt[65], w_len[65];
        hex_word64(amt + 2, strlen(amt) - 2, w_amt);
        hex_word64("2", 1, w_len);
        snprintf(data, sizeof(data), "0x%02x%02x%02x%02x%s%s%s%s%s",
                 sel[0], sel[1], sel[2], sel[3], w_amt,
                 "00000000000000000000000000000000000000000000000000000000"
                 "00000040",
                 w_len, inhex + 2, ahex + 2);
    } else {
        /* quoteExactInputSingle((in, out, amount, fee, sqrtLimit)) */
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
        snprintf(data, sizeof(data), "0x%08x%s%s%s%s%s", 0xc6a5026au,
                 w_in, w_out, w_amt, w_fee, w_zero);
    }

    /* пин блока потока: батч котируется на одном блоке (В6), latest —
     * для свежих котировок submit/monitor */
    char block_param[24];
    if (t_quote_block)
        snprintf(block_param, sizeof(block_param), "\"0x%llx\"",
                 (unsigned long long)t_quote_block);
    else
        snprintf(block_param, sizeof(block_param), "\"latest\"");

    char body[704];
    int n = snprintf(body, sizeof(body),
                     "{\"jsonrpc\":\"2.0\",\"method\":\"eth_call\","
                     "\"params\":[{\"to\":\"%s\",\"data\":\"%s\"},"
                     "%s],\"id\":1}",
                     qhex, data, block_param);
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
#define DEX_ATTACH_HOP(res_str)                                                \
    do {                                                                       \
        uint8_t pack[TB_DETAIL_DATA_MAX];                                      \
        size_t plen = tb_detail_pack_dex_hop(pack, sizeof(pack),               \
                                             venue->quoter.b, data,             \
                                             (res_str) ? (res_str) : "");      \
        tb_error_detail(err, TB_G_DEX, TB_CMD_DEX_HOP, pack, plen);            \
    } while (0)

    if (!resp)
        TB_ERROR_FAIL(err, TB_DOMAIN_CORE, TB_CORE_NO_MEMORY, "dex: resp");
    int rc = -1;
    tb_atomic_add_u64(&c->stats.rpc_calls, 1);

    if (tb_http_request(c->http, &req, resp, err) != 0) {
        tb_atomic_add_u64(&c->stats.rpc_errors, 1);
        char reason[96];
        snprintf(reason, sizeof(reason), "%s", err ? err->message : "?");
        TB_ERROR_SET(err, TB_DOMAIN_USER, TB_EXEC_BACKEND,
                     "RPC %s: %s", venue->name, reason);
        err->retryable = true; /* транспортные сбои повторяемы */
        goto out;
    }
    if (resp->status == 429 || resp->status >= 500) {
        tb_atomic_add_u64(&c->stats.rpc_errors, 1);
        tb_error_t *te = err;
        TB_ERROR_SET(te, TB_DOMAIN_USER, TB_EXEC_BACKEND,
                     "RPC %s: HTTP %ld%s", venue->name, (long)resp->status,
                     resp->retry_after_ms ? " (Retry-After)" : "");
        te->retryable = true;
        te->retry_after_ms = resp->retry_after_ms;
        {
            uint8_t pack2[TB_DETAIL_DATA_MAX];
            size_t plen2 =
                tb_detail_pack_net_raw(pack2, sizeof(pack2), resp->status,
                                       resp->body, resp->body_len);
            tb_error_detail(err, TB_G_NET, TB_CMD_NET_RAW, pack2, plen2);
        }
        goto out;
    }
    if (resp->status != 200) {
        tb_atomic_add_u64(&c->stats.rpc_errors, 1);
        TB_ERROR_SET(err, TB_DOMAIN_USER, TB_EXEC_BACKEND,
                     "RPC %s: неожиданный HTTP %ld", venue->name,
                     (long)resp->status);
        {
            uint8_t pack2[TB_DETAIL_DATA_MAX];
            size_t plen2 =
                tb_detail_pack_net_raw(pack2, sizeof(pack2), resp->status,
                                       resp->body, resp->body_len);
            tb_error_detail(err, TB_G_NET, TB_CMD_NET_RAW, pack2, plen2);
        }
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
        tb_atomic_add_u64(&c->stats.rpc_errors, 1);
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
        DEX_ATTACH_HOP(res);
        goto out;
    }
    if (getenv("TB_DEX_DEBUG"))
        fprintf(stderr, "[dex] result hex (rlen=%zu): %.134s\n", rlen, res);

    if (is_v2) {
        /* ответ Router.getAmountsOut — uint[]: [offset 0x20][len][суммы];
         * нужна последняя (выход всего пути) */
        if (rlen < 2 + 64 * 3 || (rlen - 2) % 64 != 0) {
            tb_atomic_add_u64(&c->stats.rpc_errors, 1);
            DEX_ATTACH_HOP(res);
            TB_ERROR_SET(err, TB_DOMAIN_USER, TB_EXEC_BACKEND,
                         "RPC %s: getAmountsOut ответил не массив "
                         "(rlen=%zu)",
                         venue->name, rlen);
            goto out;
        }
        /* слово 1 = длина массива; элемент i — слово (2 + i) */
        size_t arr_len = 0;
        for (size_t i = 0; i < 64; i++) {
            size_t d = hex_val(res[2 + 64 + i]);
            if (d == (size_t)-1) {
                tb_atomic_add_u64(&c->stats.rpc_errors, 1);
                DEX_ATTACH_HOP(res);
                TB_ERROR_SET(err, TB_DOMAIN_USER, TB_EXEC_BACKEND,
                             "RPC %s: getAmountsOut длина не hex",
                             venue->name);
                goto out;
            }
            arr_len = arr_len * 16 + d;
            if (arr_len > 64) { /* защита от мусора */
                tb_atomic_add_u64(&c->stats.rpc_errors, 1);
                DEX_ATTACH_HOP(res);
                TB_ERROR_SET(err, TB_DOMAIN_USER, TB_EXEC_BACKEND,
                             "RPC %s: getAmountsOut длина безумна (%zu)",
                             venue->name, arr_len);
                goto out;
            }
        }
        if (arr_len == 0 || (rlen - 2) / 64 < arr_len + 2) {
            tb_atomic_add_u64(&c->stats.rpc_errors, 1);
            DEX_ATTACH_HOP(res);
            TB_ERROR_SET(err, TB_DOMAIN_USER, TB_EXEC_BACKEND,
                         "RPC %s: getAmountsOut короче длины (%zu)",
                         venue->name, arr_len);
            goto out;
        }
        /* последнее слово массива -> out */
        *out = tb_u256_from_u64(0);
        const char *last = res + 2 + 64 * (1 + arr_len);
        for (size_t i = 0; i < 64; i++) {
            size_t d = hex_val(last[i]);
            if (d == (size_t)-1) {
                tb_atomic_add_u64(&c->stats.rpc_errors, 1);
                DEX_ATTACH_HOP(res);
                TB_ERROR_SET(err, TB_DOMAIN_USER, TB_EXEC_BACKEND,
                             "RPC %s: getAmountsOut элемент не hex",
                             venue->name);
                goto out;
            }
            (void)tb_u256_mul_u64(out, out, 16);
            tb_u256_t add = tb_u256_from_u64(d);
            (void)tb_u256_add(out, out, &add);
        }
    } else if (hex_to_u256(out, res, rlen > 66 ? 66 : rlen, err) != 0) {
        /* ответ QuoterV2 — 4 слова; слово 0 (первые 32 байта) = amountOut */
        tb_atomic_add_u64(&c->stats.rpc_errors, 1);
        DEX_ATTACH_HOP(res);
        TB_ERROR_WRAP(err, "RPC %s", venue->name);
        goto out;
    }
    if (tb_u256_is_zero(out)) {
        tb_atomic_add_u64(&c->stats.rpc_errors, 1);
        TB_ERROR_SET(err, TB_DOMAIN_USER, TB_EXEC_INSUFFICIENT_LIQUIDITY,
                     "RPC %s: нулевая котировка (rlen=%zu)", venue->name,
                     rlen);
        DEX_ATTACH_HOP(res); /* слепая зона "0x": сырой ответ в капсуле */
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
            TB_ERROR_WRAP(err, "хоп %zu (%s)", h, venue->name);
            return -1;
        }
        v = *out;
    }
    tb_atomic_add_u64(&c->stats.quotes, 1);
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
    tb_atomic_add_u64(&c->stats.submits, 1);

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

    if (c->cfg.live) {
        /* ---- live = propose-only (P0): подготовить SafeTx для подписи
         * человеком; ничего не отправляем. Исполнение человек делает
         * второй подписью в кошельке; результат узнаем ростом nonce
         * и дельтой баланса. ---- */
        dex_session_t *s = NULL;
        for (size_t i = 0; i < TB_DEX_MAX_SESSIONS; i++)
            if (c->txs[i].tx_id == 0) {
                s = &c->txs[i];
                break;
            }
        if (!s)
            TB_ERROR_FAIL(err, TB_DOMAIN_USER, TB_EXEC_CAPACITY,
                          "dex: таблица сессий полна");

        tb_addr_t out_tok = tb_route_last(route);
        tb_u256_t before;
        tb_error_t berr = TB_ERROR_INIT;
        if (tb_safe_balance_of(c->http, &c->cfg.safe, &out_tok,
                               &c->cfg.safe.safe, &before, &berr) != 0) {
            *err = berr;
            return -1;
        }

        uint8_t calldata[2048];
        size_t calldata_len = tb_safe_build_exact_input(
            calldata, sizeof(calldata), route, &c->cfg.safe.safe,
            amount_in, min_out ? min_out : &out);
        if (!calldata_len)
            TB_ERROR_FAIL(err, TB_DOMAIN_CORE, TB_CORE_OVERFLOW,
                          "dex: exactInput calldata");

        tb_safe_proposal_t prop;
        if (tb_safe_propose_exec(c->http, &c->cfg.safe, &c->cfg.safe.router,
                                 0, calldata, calldata_len, now_ms, &prop,
                                 err) != 0)
            return -1;
        tb_atomic_add_u64(&c->stats.proposals, 1);
        if (c->cfg.on_propose)
            c->cfg.on_propose(&prop, c->cfg.propose_user);

        memset(s, 0, sizeof(*s));
        s->tx_id = ++c->next_tx;
        s->live = true;
        s->safe_nonce = prop.safe_nonce;
        s->expires_ms = prop.expires_at_ms;
        s->out_token = out_tok;
        s->balance_before = before;
        s->amount_out = out; /* оценка; истина придёт дельтой в poll */

        *tx_id = s->tx_id;
        *amount_out = out;
        return 1; /* ПРЕДЛОЖЕНО: ждём подписей и исполнения человеком */
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

    if (s->live) {
        /* Предложение ждёт человека: следим за nonce Safe. Истекло окно
         * — отказ: на цепи ничего не было, газ не потрачен. */
        if (now_ms > s->expires_ms) {
            s->tx_id = 0;
            TB_ERROR_FAIL(err, TB_DOMAIN_USER, TB_EXEC_BACKEND,
                          "dex: предложение истекло без подписей "
                          "(на цепи ничего не произошло, газ цел)");
        }

        uint64_t nonce = 0;
        if (tb_safe_nonce(c->http, &c->cfg.safe, &nonce, err) != 0)
            return -1;
        if (nonce <= s->safe_nonce)
            return 1; /* ещё не исполнено */

        /* nonce вырос: человек исполнил (или вытеснил нашу транзакцию —
         * дельта покажет факту без прикрас). Истина с цепи. */
        tb_u256_t after;
        tb_error_t berr = TB_ERROR_INIT;
        if (tb_safe_balance_of(c->http, &c->cfg.safe, &s->out_token,
                               &c->cfg.safe.safe, &after, &berr) != 0) {
            *err = berr;
            return -1;
        }
        if (!tb_u256_sub(amount_out, &after, &s->balance_before))
            memset(amount_out, 0, sizeof(*amount_out)); /* стало меньше */

        s->tx_id = 0;
        tb_atomic_add_u64(&c->stats.fills, 1);
        return 0; /* исполнено человеком: фактическая дельта */
    }

    if (now_ms < s->fill_at_ms)
        return 1; /* ещё в работе */

    *amount_out = s->amount_out;
    s->tx_id = 0;
    tb_atomic_add_u64(&c->stats.fills, 1);
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
        /* имя — ДО free(c): TB_ERROR_FAIL форматирует сообщение, а
         * venues живут внутри освобождаемого ctx (ASan ловил UAF) */
        char vname[TB_DEX_NAME_MAX];
        snprintf(vname, sizeof(vname), "%s", c->cfg.venues[v].name);
        if (!c->cfg.venues[v].rpc_url[0] || c->cfg.venues[v].pools_n == 0) {
            free(c);
            TB_ERROR_FAIL(err, TB_DOMAIN_CORE, TB_CORE_INVALID_ARG,
                          "dex: venue %u без rpc_url или пулов", v);
        }
        /* метод котировки обязан быть настроен (fail-fast, не на хопе) */
        if (c->cfg.venues[v].kind == TB_DEX_V2
            && tb_addr_is_native(&c->cfg.venues[v].router)) {
            free(c);
            TB_ERROR_FAIL(err, TB_DOMAIN_CORE, TB_CORE_INVALID_ARG,
                          "dex: venue %u (%s, V2) без router", v, vname);
        }
        if (c->cfg.venues[v].kind != TB_DEX_V2
            && tb_addr_is_native(&c->cfg.venues[v].quoter)) {
            free(c);
            TB_ERROR_FAIL(err, TB_DOMAIN_CORE, TB_CORE_INVALID_ARG,
                          "dex: venue %u (%s, V3) без quoter", v, vname);
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

/* ==========================================================================
 *  Стартерные вызовы к цепи (В4/В5/В6): блокирующие, только на старте или
 *  из воркеров пула. Транспорт/RPC — из venue; бэкенд не нужен.
 * ========================================================================== */

/* POST JSON-RPC и вынуть поле "result" (hex-строка без изменений) в буфер
 * вызывающего. 0 — донесено (в т.ч. "0x" — пустой ответ). */
static int rpc_result(tb_http_t *http, const tb_exec_dex_venue_t *v,
                      const char *body, char *result, size_t result_cap,
                      tb_error_t *err)
{
    tb_http_req_t req = {0};
    strcpy(req.method, "POST");
    strcpy(req.url, v->rpc_url);
    if (v->rpc_host_pinned[0])
        strcpy(req.host_pinned, v->rpc_host_pinned);
    req.body = body;
    req.body_len = strlen(body);
    req.timeout_ms = v->timeout_ms ? v->timeout_ms : 8000;

    tb_http_resp_t *resp = calloc(1, sizeof(*resp));
    if (!resp)
        TB_ERROR_FAIL(err, TB_DOMAIN_CORE, TB_CORE_NO_MEMORY, "rpc_result");

    if (tb_http_request(http, &req, resp, err) != 0) {
        free(resp);
        return -1; /* транспорт уже заполнил err (retryable сохранён) */
    }
    if (resp->status != 200) {
        bool transient = resp->status == 429 || resp->status >= 500;
        TB_ERROR_SET(err, TB_DOMAIN_USER, TB_EXEC_BACKEND,
                     "RPC %s: HTTP %ld", v->name, (long)resp->status);
        err->retryable = transient;
        free(resp);
        return -1;
    }
    resp->body[resp->body_len < TB_HTTP_BODY_MAX ? resp->body_len
                                                 : TB_HTTP_BODY_MAX - 1] = '\0';

    size_t rlen = 0;
    const char *res = json_str_field(resp->body, "result", &rlen);
    if (!res || json_has_error(resp->body)) {
        size_t mlen = 0;
        const char *msg = json_str_field(resp->body, "message", &mlen);
        char mbuf[128] = "нет сообщения";
        if (msg && mlen < sizeof(mbuf)) {
            memcpy(mbuf, msg, mlen);
            mbuf[mlen] = '\0';
        }
        TB_ERROR_SET(err, TB_DOMAIN_USER, TB_EXEC_BACKEND, "RPC %s: %s",
                     v->name, mbuf);
        free(resp);
        return -1;
    }
    if (rlen >= result_cap)
        rlen = result_cap - 1;
    memcpy(result, res, rlen);
    result[rlen] = '\0';
    free(resp);
    return 0;
}

/* eth_call(to, data) -> result hex. */
static int chain_call(tb_http_t *http, const tb_exec_dex_venue_t *v,
                      const tb_addr_t *to, const char *data_hex, char *result,
                      size_t result_cap, tb_error_t *err)
{
    char tohex[43], body[512]; /* to + селектор + до трёх ABI-слов */
    tb_addr_to_hex(to, tohex, sizeof(tohex));
    int n = snprintf(body, sizeof(body),
                     "{\"jsonrpc\":\"2.0\",\"method\":\"eth_call\","
                     "\"params\":[{\"to\":\"%s\",\"data\":\"%s\"},"
                     "\"latest\"],\"id\":1}",
                     tohex, data_hex);
    if (n < 0 || (size_t)n >= sizeof(body))
        TB_ERROR_FAIL(err, TB_DOMAIN_USER, TB_EXEC_BACKEND, "eth_call: calldata");
    return rpc_result(http, v, body, result, result_cap, err);
}

int tb_exec_dex_block_number(tb_http_t *http, const tb_exec_dex_venue_t *v,
                             uint64_t *block, tb_error_t *err)
{
    char result[80];
    if (rpc_result(http, v,
                   "{\"jsonrpc\":\"2.0\",\"method\":\"eth_blockNumber\","
                   "\"params\":[],\"id\":1}",
                   result, sizeof(result), err) != 0)
        return -1;
    size_t len = strlen(result);
    if (len < 2 || result[0] != '0' || result[1] != 'x' || len == 2)
        TB_ERROR_FAIL(err, TB_DOMAIN_USER, TB_EXEC_BACKEND,
                      "eth_blockNumber: некорректный результат '%.32s'",
                      result);
    *block = strtoull(result + 2, NULL, 16);
    return 0;
}

int tb_exec_dex_gas_price(tb_http_t *http, const tb_exec_dex_venue_t *v,
                          uint64_t *price_wei, tb_error_t *err)
{
    char result[80];
    if (rpc_result(http, v,
                   "{\"jsonrpc\":\"2.0\",\"method\":\"eth_gasPrice\","
                   "\"params\":[],\"id\":1}",
                   result, sizeof(result), err) != 0)
        return -1;
    size_t len = strlen(result);
    if (len < 3 || result[0] != '0' || result[1] != 'x')
        TB_ERROR_FAIL(err, TB_DOMAIN_USER, TB_EXEC_BACKEND,
                      "eth_gasPrice: некорректный результат '%.32s'",
                      result);
    uint64_t base = strtoull(result + 2, NULL, 16);

    /* приоритетная премия опциональна: не все узлы дают метод */
    uint64_t prio = 0;
    tb_error_t perr = TB_ERROR_INIT;
    if (rpc_result(http, v,
                   "{\"jsonrpc\":\"2.0\",\"method\":"
                   "\"eth_maxPriorityFeePerGas\",\"params\":[],\"id\":1}",
                   result, sizeof(result), &perr) == 0
        && strlen(result) > 2 && result[0] == '0' && result[1] == 'x')
        prio = strtoull(result + 2, NULL, 16);

    unsigned __int128 total = (unsigned __int128)base + prio;
    *price_wei = total >> 64 ? UINT64_MAX : (uint64_t)total;
    return 0;
}

int tb_exec_dex_verify_pool(tb_http_t *http, const tb_exec_dex_venue_t *v,
                            const tb_addr_t *token_a, const tb_addr_t *token_b,
                            uint32_t fee, const tb_addr_t *expect_pool,
                            tb_error_t *err)
{
    if (tb_addr_is_native(&v->factory))
        return 0; /* фабрика не задана: проверять нечем (с предупреждением
                     на совести вызывающего) */

    const bool is_v2 = v->kind == TB_DEX_V2;
    uint8_t sel[4];
    if (is_v2)
        tb_keccak_selector("getPair(address,address)", sel);
    else
        tb_keccak_selector("getPool(address,address,uint24)", sel);

    char ahex[43], bhex[43], wa[65], wb[65], wfee[65];
    tb_addr_to_hex(token_a, ahex, sizeof(ahex));
    tb_addr_to_hex(token_b, bhex, sizeof(bhex));
    hex_word64(ahex + 2, 40, wa);
    hex_word64(bhex + 2, 40, wb);

    char data[8 + 64 * 3 + 3];
    if (is_v2) {
        snprintf(data, sizeof(data), "0x%02x%02x%02x%02x%s%s", sel[0],
                 sel[1], sel[2], sel[3], wa, wb);
    } else {
        char feehex[17];
        snprintf(feehex, sizeof(feehex), "%x", fee);
        hex_word64(feehex, strlen(feehex), wfee);
        snprintf(data, sizeof(data), "0x%02x%02x%02x%02x%s%s%s", sel[0],
                 sel[1], sel[2], sel[3], wa, wb, wfee);
    }

    char result[80];
    if (chain_call(http, v, &v->factory, data, result, sizeof(result),
                   err) != 0) {
        TB_ERROR_WRAP(err, "%s %s", is_v2 ? "getPair" : "getPool", v->name);
        return -1;
    }

    /* ответ — слово; адрес = последние 20 байт (40 hex-символов) */
    tb_addr_t on_chain;
    size_t len = strlen(result);
    char expect_hex[43], got_hex[43];
    tb_addr_to_hex(expect_pool, expect_hex, sizeof(expect_hex));
    const char *fn = is_v2 ? "getPair" : "getPool";
    if (len <= 2) { /* "0x" — фабрика не знает такой пары/fee */
        TB_ERROR_SET(err, TB_DOMAIN_USER, TB_EXEC_BACKEND,
                     "%s %s: пула %s нет (фабрика вернула 0x0; "
                     "пара/fee в конфиге не совпадают с цепью)",
                     fn, v->name, expect_hex);
        return -1;
    }
    if (len < 42 || (len - 2) % 2 != 0) {
        TB_ERROR_SET(err, TB_DOMAIN_USER, TB_EXEC_BACKEND,
                     "%s %s: ответ не слово '%.32s'", fn, v->name, result);
        return -1;
    }
    char addr_hex[43] = "0x";
    memcpy(addr_hex + 2, result + len - 40, 40);
    if (tb_addr_from_hex(&on_chain, addr_hex, err) != 0) {
        TB_ERROR_WRAP(err, "%s %s", fn, v->name);
        return -1;
    }
    tb_addr_to_hex(&on_chain, got_hex, sizeof(got_hex));
    if (!tb_addr_eq(&on_chain, expect_pool)) {
        TB_ERROR_SET(err, TB_DOMAIN_USER, TB_EXEC_BACKEND,
                     "%s %s: конфиг 0x%s, фабрика 0x%s",
                     fn, v->name, expect_hex + 2, got_hex + 2);
        return -1;
    }
    return 0;
}

/* hex-символ -> значение; -1 — не hex */
static int nib(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* слово ответа ABI (64 hex-символа) по индексу -> u64 (значение слова;
 * ведущие нули естественны, переполнение u64 отсекается проверками выше) */
static bool word_u64(const char *hex, size_t nibbles, size_t idx, uint64_t *out)
{
    size_t want = (idx + 1) * 64;
    if (want > nibbles)
        return false;
    uint64_t v = 0;
    for (size_t i = idx * 64; i < want; i++) {
        int d = nib(hex[i]);
        if (d < 0)
            return false;
        v = (v << 4) | (uint64_t)d;
    }
    *out = v;
    return true;
}

int tb_exec_dex_token_info(tb_http_t *http, const tb_exec_dex_venue_t *v,
                           const tb_addr_t *token, char *symbol,
                           size_t symbol_cap, uint8_t *decimals,
                           tb_error_t *err)
{
    uint8_t sel_sym[4], sel_dec[4];
    tb_keccak_selector("symbol()", sel_sym);
    tb_keccak_selector("decimals()", sel_dec);

    char data[13];
    char result[320]; /* до четырёх ABI-слов (длинные имена/массивы) */
    symbol[0] = '\0';
    *decimals = 0;

    snprintf(data, sizeof(data), "0x%02x%02x%02x%02x", sel_sym[0], sel_sym[1],
             sel_sym[2], sel_sym[3]);
    if (chain_call(http, v, token, data, result, sizeof(result), err) != 0)
        return -1;

    size_t len = strlen(result);
    if (len <= 2 || result[0] != '0' || result[1] != 'x'
        || (len % 2) != 0) {
        TB_ERROR_SET(err, TB_DOMAIN_USER, TB_EXEC_BACKEND,
                     "symbol(): некорректный ответ '%.24s'", result);
        return -1;
    }
    const char *hex = result + 2;
    size_t nibbles = len - 2;

    if (nibbles == 64) {
        /* старые токены: bytes32 — байты до первого нуля */
        size_t o = 0;
        for (size_t w = 0; w < 32 && o + 1 < symbol_cap; w++) {
            int hi = nib(hex[w * 2]), lo = nib(hex[w * 2 + 1]);
            if (hi < 0 || lo < 0)
                break;
            char c = (char)((hi << 4) | lo);
            if (c == '\0')
                break;
            symbol[o++] = c;
        }
        symbol[o] = '\0';
    } else {
        /* строка ABI: слово0 = смещение (0x20), слово1 = длина */
        uint64_t off, slen;
        if (!word_u64(hex, nibbles, 0, &off) || off != 0x20
            || !word_u64(hex, nibbles, 1, &slen)) {
            TB_ERROR_SET(err, TB_DOMAIN_USER, TB_EXEC_BACKEND,
                         "symbol(): не строка ABI");
            return -1;
        }
        size_t avail = (nibbles - 128) / 2; /* байты после двух слов */
        if (slen > avail)
            slen = avail;
        if (slen > symbol_cap - 1)
            slen = symbol_cap - 1;
        for (size_t b = 0; b < slen; b++) {
            int hi = nib(hex[128 + b * 2]), lo = nib(hex[128 + b * 2 + 1]);
            if (hi < 0 || lo < 0) {
                TB_ERROR_SET(err, TB_DOMAIN_USER, TB_EXEC_BACKEND,
                             "symbol(): не hex в данных строки");
                return -1;
            }
            symbol[b] = (char)((hi << 4) | lo);
        }
        symbol[slen] = '\0';
    }
    if (!symbol[0]) {
        TB_ERROR_SET(err, TB_DOMAIN_USER, TB_EXEC_BACKEND,
                     "symbol(): пустой символ у токена");
        return -1;
    }

    snprintf(data, sizeof(data), "0x%02x%02x%02x%02x", sel_dec[0], sel_dec[1],
             sel_dec[2], sel_dec[3]);
    if (chain_call(http, v, token, data, result, sizeof(result), err) != 0)
        return -1;
    len = strlen(result);
    if (len <= 2 || (len - 2) > 128) { /* одно слово; hex может быть
                                        * минимальным ("0x6" = 6) */
        TB_ERROR_SET(err, TB_DOMAIN_USER, TB_EXEC_BACKEND,
                     "decimals(): некорректный ответ '%.24s'", result);
        return -1;
    }
    uint64_t dec = 0;
    for (size_t i = 2; i < len; i++) {
        int d = nib(result[i]);
        if (d < 0) {
            TB_ERROR_SET(err, TB_DOMAIN_USER, TB_EXEC_BACKEND,
                         "decimals(): не hex '%.24s'", result);
            return -1;
        }
        dec = (dec << 4) | (uint64_t)d;
    }
    if (dec > 255) {
        TB_ERROR_SET(err, TB_DOMAIN_USER, TB_EXEC_BACKEND,
                     "decimals(): бессмыслица %llu", (unsigned long long)dec);
        return -1;
    }
    *decimals = (uint8_t)dec;
    return 0;
}
