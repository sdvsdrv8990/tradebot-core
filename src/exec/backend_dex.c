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
    bool       superseded; /* заменено новым предложением (тот же nonce) */
    uint64_t   safe_nonce; /* nonce Safe на момент предложения */
    uint64_t   expires_ms; /* окно наблюдения; истекло — отказ без цепи */
    uint8_t    safe_tx_hash[32]; /* атрибуция исполнения (H1.5) */
    uint64_t   born_block;  /* блок создания (getLogs от него) */
    tb_addr_t  out_token;  /* последний токен маршрута */
    tb_u256_t  balance_before; /* для дельты после роста nonce */
} dex_session_t;

/* Дисциплина p0.1-B: одно живое предложение на Safe. Пока человек не
 * исполнил (nonce не вырос) и окно не истекло, новое предложение
 * ЗАМЕНЯЕТ старое (тот же nonce); параллельные конкурирующие nonce не
 * постим. Живой неподписанный approve блокирует trade-предложения. */
typedef struct dex_live_prop {
    bool      active;        /* живо предложение (trade или approve) */
    bool      approve;       /* живой approve (торговля заблокирована) */
    uint64_t  safe_nonce;
    uint64_t  expires_ms;
    uint64_t  tx_id;         /* сессия, чьё предложение живо */
} dex_live_prop_t;

typedef struct dex_ctx {
    tb_exec_dex_cfg_t cfg;
    tb_http_t        *http;
    uint64_t          rand_state;
    uint64_t          next_tx;
    dex_session_t     txs[TB_DEX_MAX_SESSIONS];
    dex_live_prop_t   live; /* поток-владелец движка */
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


/* ---- Сегменты маршрута (R2): хопы одной биржи -> один exactInput ---- */

struct seg_ref {
    size_t begin, end; /* хопы [begin, end) */
    tb_addr_t router;
};

/* Суб-маршрут сегмента: токены/ф/пулы хопов [begin,end). */
static tb_route_t seg_subroute(const tb_route_t *route,
                               const struct seg_ref *s, tb_error_t *err)
{
    tb_addr_t toks[TB_EXEC_MAX_HOPS + 1];
    uint32_t fees[TB_EXEC_MAX_HOPS];
    size_t n = s->end - s->begin;
    for (size_t h = 0; h < n; h++)
        fees[h] = tb_route_fee(route, s->begin + h);
    for (size_t t = 0; t <= n; t++)
        toks[t] = tb_route_token(route, s->begin + t);
    tb_route_t sub = tb_route_encode(toks, fees, n + 1, err);
    if (err->code != 0)
        return sub;
    for (size_t h = 0; h < n; h++) {
        tb_addr_t p = tb_route_pool(route, s->begin + h);
        tb_route_set_pool(&sub, h, &p);
    }
    return sub;
}

/* Котировка сегмента (цепочка хопов сегмента). */
static int dex_quote_segment(dex_ctx_t *c, const tb_route_t *route,
                             const struct seg_ref *s, const tb_u256_t *in,
                             tb_u256_t *out, tb_error_t *err)
{
    tb_error_t re = TB_ERROR_INIT;
    tb_route_t sub = seg_subroute(route, s, &re);
    if (re.code != 0) {
        *err = re;
        return -1;
    }
    return dex_quote_route(c, &sub, in, out, err);
}

/* ratio_bp = a*10000/b (b>0, результат <= 0xffff), ложь если вне */
static bool seg_ratio_bp(const tb_u256_t *a, const tb_u256_t *b,
                         tb_u256_t *out)
{
    if (tb_u256_is_zero(b))
        return false;
    tb_u256_t x = *a, y = *b;
    while (y.w[1] || y.w[2] || y.w[3]) {
        x.w[0] = x.w[1]; x.w[1] = x.w[2]; x.w[2] = x.w[3]; x.w[3] = 0;
        y.w[0] = y.w[1]; y.w[1] = y.w[2]; y.w[2] = y.w[3]; y.w[3] = 0;
        if (tb_u256_is_zero(&y))
            return false;
    }
    if (!tb_u256_mul_u64(&x, &x, 10000))
        return false;
    (void)tb_u256_div_u64(&x, &x, y.w[0]);
    if (x.w[1] || x.w[2] || x.w[3] || x.w[0] > 0xffffull)
        return false;
    *out = x;
    return true;
}

/* need_prev = expected_prev * ratio(need_next/expected_next, +1 б.п.)
 * Целочисленно в u128 (наши суммы < 2^120); финальный пол ордера
 * держит итоговую гарантию — точности б.п. достаточно. */
/* need_prev = expected_prev * ratio(need_next/expected_next, +1 б.п.)
 * Честная u256-арифметика: u128-произведение могло переполниться ДО
 * проверки (ревью R3) — молчаливая деградация минимума до 1 wei
 * ослабляла промежуточную защиту почти до нуля. Теперь:
 * out = expected_prev * ratio / 10000 в 256 битах; переполнение или
 * невозможность посчитать = отказ строить предложение целиком. */
static bool seg_scale(tb_u256_t *out, const tb_u256_t *expected_prev,
                      const tb_u256_t *need_next,
                      const tb_u256_t *expected_next)
{
    tb_u256_t ratio;
    if (!seg_ratio_bp(need_next, expected_next, &ratio))
        return false;
    if (!tb_u256_mul_u64(&ratio, &ratio, 10001)) /* +1 б.п. запас */
        return false;
    (void)tb_u256_div_u64(&ratio, &ratio, 10000); /* обратно в б.п.:
                                                    * потеря этой строки
                                                    * давала ratio x10000
                                                                    */
    /* out = expected_prev * ratio / 10000 без 256x256-умножителя:
     * разложение по словам со сбором частичного переполнения.
     * ratio <= ~10101 (<< 2^14): произведение точного expected_prev
     * на слово ratio поместится в u256 c запасом 2^242 */
    tb_u256_t acc = tb_u256_from_u64(0);
    tb_u256_t part;
    for (int w = 0; w < 4; w++) {
        if (!ratio.w[w])
            continue;
        /* part = expected_prev * ratio.w[w] (сдвиг слова) */
        uint64_t carry = 0;
        for (int k = 0; k + w < 4; k++) {
            unsigned __int128 p =
                (unsigned __int128)expected_prev->w[k] * ratio.w[w]
                + carry;
            part.w[k] = (uint64_t)p;
            carry = (uint64_t)(p >> 64);
        }
        if (carry && w)
            return false; /* старшее слово вылезло — переполнение */
        for (int k = 4 - w; k < 4; k++)
            part.w[k] = 0;
        /* сдвиг на w слов влево */
        if (w) {
            for (int k = 3; k >= w; k--)
                part.w[k] = part.w[k - w];
            for (int k = w - 1; k >= 0; k--)
                part.w[k] = 0;
        }
        if (!tb_u256_add(&acc, &acc, &part))
            return false; /* суммарное переполнение 256 бит */
    }
    if (!tb_u256_div_u64(out, &acc, 10000))
        return false;
    return !tb_u256_is_zero(out);
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
        /* ---- live = propose-only, дисциплина p0.1-B:
         * 1) allowance-гейт: не хватает на ставку → предложить approve
         *    (если он ещё не жив) и ОТКАЗАТЬ trade;
         * 2) живой approve → trade не предлагается вовсе;
         * 3) одно живое предложение: новое заменяет старое (тот же
         *    nonce), старая сессия помечается superseded;
         * 4) nonce на цепи вырос → прежнее исполнено, состояние
         *    сбрасывается (allowance перечитается этим же гейтом). ---- */
        dex_live_prop_t *lp = &c->live;

        /* nonce на цепи: вырос — прежнее живое предложение исполнено */
        {
            uint64_t nonce = 0;
            tb_error_t nerr = TB_ERROR_INIT;
            if (tb_safe_nonce(c->http, &c->cfg.safe, &nonce, &nerr) != 0) {
                *err = nerr;
                return -1;
            }
            if (lp->active && nonce > lp->safe_nonce) {
                lp->active = false;
                lp->approve = false; /* исполнено (approve в том числе) */
            }
            if (lp->active && now_ms > lp->expires_ms) {
                lp->active = false; /* окно истекло — не ждём */
                lp->approve = false;
            }
        }

        /* allowance-гейт перед КАЖДЫМ trade-предложением */
        tb_u256_t allow;
        tb_error_t aerr = TB_ERROR_INIT;
        tb_addr_t in_tok = tb_route_token(route, 0);
        if (tb_safe_allowance(c->http, &c->cfg.safe, &in_tok,
                              &c->cfg.safe.safe, &c->cfg.safe.router,
                              &allow, &aerr) != 0) {
            *err = aerr;
            return -1;
        }
        if (tb_u256_cmp(&allow, amount_in) < 0) {
            tb_atomic_add_u64(&c->stats.allowance_gates, 1);
            if (lp->active && lp->approve)
                TB_ERROR_FAIL(err, TB_DOMAIN_USER, TB_EXEC_BACKEND,
                              "dex: allowance ниже ставки — approve уже "
                              "предложен, подпиши его в Safe UI");
            /* предложить approve на рабочий лимит и отказать trade */
            tb_u256_t cap = c->cfg.approve_cap_wei
                                ? tb_u256_from_u64(c->cfg.approve_cap_wei)
                                : *amount_in;
            uint8_t cd[4 + 64];
            size_t cd_len = tb_safe_build_approve(cd, sizeof(cd),
                                                  &c->cfg.safe.router,
                                                  &cap);
            tb_safe_proposal_t ap;
            if (cd_len
                && tb_safe_propose_exec(c->http, &c->cfg.safe, &in_tok, 0,
                                        cd, cd_len, now_ms, &ap,
                                        err) == 0) {
                snprintf(ap.kind, sizeof(ap.kind), "approve");
                tb_atomic_add_u64(&c->stats.proposals, 1);
                if (c->cfg.on_propose)
                    c->cfg.on_propose(&ap, c->cfg.propose_user);
                lp->active = true;
                lp->approve = true;
                lp->safe_nonce = ap.safe_nonce;
                lp->expires_ms = ap.expires_at_ms;
                lp->tx_id = 0;
            }
            TB_ERROR_FAIL(err, TB_DOMAIN_USER, TB_EXEC_BACKEND,
                          "dex: allowance WETH->роутера ниже ставки — "
                          "предложен approve, подпиши в Safe UI");
        }

        /* живой approve без исполнения: trade в очередь не ставим */
        if (lp->active && lp->approve)
            TB_ERROR_FAIL(err, TB_DOMAIN_USER, TB_EXEC_BACKEND,
                          "dex: ждём подписи approve — trade не "
                          "предлагается");

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
        size_t calldata_len = 0; /* заполняет сегментация ниже */

        /* Целостность маршрута (R1+R2): path exactInput резолвится
         * фабрикой РОУТЕРА — хопы чужих фабрик обязаны идти через
         * venue.router своей биржи. Сегментируем хопы по роутерам:
         * один сегмент — прямой exactInput; несколько — пачка в
         * MultiSendCallOnly (одна транзакция Safe; msg.sender
         * внутренних вызовов = Safe — transferFrom роутеров работает;
         * отказ любой ноги ревертит всё). Минимумы: последний сегмент
         * — пол ордера; промежуточные — ожидаемый выход,
         * промасштабированный до доли final_min/final_expected
         * (+1 б.п. запас) — двухуровневая защита исходного аудита */
        tb_addr_t exec_to = c->cfg.safe.router; /* цель execTransaction */
        {
            tb_addr_t rf = c->cfg.router_factory;
            if (tb_addr_is_native(&rf)) {
                tb_error_t fe = TB_ERROR_INIT;
                if (tb_addr_from_hex(&rf,
                                     "0x1F98431c8aD98523631AE4a59f267"
                                     "346ea31F984",
                                     &fe)
                    != 0)
                    rf = c->cfg.safe.router;
            }

            enum { MAXSEG = TB_EXEC_MAX_HOPS };
            struct seg_ref segs[MAXSEG];
            size_t segs_n = 0;
            for (size_t h = 0; h < tb_route_hops(route); h++) {
                tb_addr_t pool = tb_route_pool(route, h);
                const tb_exec_dex_venue_t *v = venue_by_pool(c, &pool);
                if (!v)
                    TB_ERROR_FAIL(err, TB_DOMAIN_USER, TB_EXEC_BACKEND,
                                  "маршрут: пул хопа %zu вне бирж", h);
                tb_addr_t router;
                if (!tb_addr_is_native(&v->factory)
                    && tb_addr_eq(&v->factory, &rf)) {
                    router = c->cfg.safe.router; /* фабрика роутера */
                } else if (!tb_addr_is_native(&v->router)) {
                    router = v->router; /* роутер своей биржи (R2) */
                } else {
                    TB_ERROR_FAIL(err, TB_DOMAIN_USER, TB_EXEC_BACKEND,
                                  "маршрут: хоп %zu (%s) на чужой "
                                  "фабрике без venue.router — исполнить "
                                  "тем же пулом нельзя",
                                  h, v->name);
                }
                if (segs_n
                    && tb_addr_eq(&segs[segs_n - 1].router, &router)) {
                    segs[segs_n - 1].end = h + 1;
                } else {
                    segs[segs_n].begin = h;
                    segs[segs_n].end = h + 1;
                    segs[segs_n].router = router;
                    segs_n++;
                }
            }

            /* ожидаемые выходы сегментов: цепочка amountIn */
            tb_u256_t expected[MAXSEG];
            {
                tb_u256_t amt = *amount_in;
                for (size_t s = 0; s < segs_n; s++) {
                    tb_error_t qe = TB_ERROR_INIT;
                    if (dex_quote_segment(c, route, &segs[s], &amt,
                                          &expected[s], &qe) != 0) {
                        *err = qe;
                        return -1;
                    }
                    amt = expected[s];
                }
            }

            /* цепочка минимумов: need[last] = пол ордера; предыдущие —
             * доля ожидаемого (final_min/final_expected, +1 б.п.) */
            tb_u256_t need_out[MAXSEG];
            need_out[segs_n - 1] = min_out ? *min_out : out;
            for (size_t s = segs_n - 1; s > 0; s--) {
                if (!seg_scale(&need_out[s - 1], &expected[s - 1],
                               &need_out[s], &expected[s]))
                    /* ревью R3: промежуточный минимум обязан быть
                     * рассчитан; молчаливая единица ослабляла защиту —
                     * отказываем строить предложение целиком */
                    TB_ERROR_FAIL(err, TB_DOMAIN_USER, TB_EXEC_BACKEND,
                                  "маршрут: промежуточный минимум "
                                  "сегмента %zu не считается (арифметика)",
                                  s - 1);
            }

            if (segs_n == 1) {
                calldata_len = tb_safe_build_exact_input(
                    calldata, sizeof(calldata), route, &c->cfg.safe.safe,
                    amount_in, &need_out[0]);
                if (!calldata_len)
                    TB_ERROR_FAIL(err, TB_DOMAIN_CORE, TB_CORE_OVERFLOW,
                                  "dex: exactInput calldata");
            } else {
                tb_addr_t ms = c->cfg.multisend;
                if (tb_addr_is_native(&ms)) {
                    tb_error_t me = TB_ERROR_INIT;
                    if (tb_addr_from_hex(&ms, TB_SAFE_MULTISEND_CALLONLY,
                                         &me)
                        != 0)
                        TB_ERROR_FAIL(err, TB_DOMAIN_USER, TB_EXEC_BACKEND,
                                      "маршрут: адрес MultiSend");
                }
                uint8_t legs_buf[MAXSEG][2048];
                tb_safe_call_t calls[MAXSEG];
                size_t legs = 0;
                tb_u256_t amt = *amount_in;
                for (size_t s = 0; s < segs_n; s++) {
                    tb_error_t qe2 = TB_ERROR_INIT;
                    tb_u256_t seg_out;
                    if (dex_quote_segment(c, route, &segs[s], &amt,
                                          &seg_out, &qe2) != 0) {
                        *err = qe2;
                        return -1;
                    }
                    tb_error_t re = TB_ERROR_INIT;
                    tb_route_t sub = seg_subroute(route, &segs[s], &re);
                    if (re.code != 0 || !sub.len)
                        TB_ERROR_FAIL(err, TB_DOMAIN_CORE,
                                      TB_CORE_OVERFLOW,
                                      "маршрут: суб-путь %zu", s);
                    size_t ln = tb_safe_build_exact_input(
                        legs_buf[s], sizeof(legs_buf[s]), &sub,
                        &c->cfg.safe.safe, &amt, &need_out[s]);
                    if (!ln)
                        TB_ERROR_FAIL(err, TB_DOMAIN_CORE,
                                      TB_CORE_OVERFLOW,
                                      "маршрут: calldata сегмента %zu", s);
                    calls[legs].to = segs[s].router;
                    calls[legs].value = 0;
                    calls[legs].data = legs_buf[s];
                    calls[legs].data_len = ln;
                    legs++;
                    amt = seg_out;
                }
                calldata_len = tb_safe_build_multisend(
                    calldata, sizeof(calldata), calls, legs);
                if (!calldata_len)
                    TB_ERROR_FAIL(err, TB_DOMAIN_CORE, TB_CORE_OVERFLOW,
                                  "маршрут: multiSend calldata");
                exec_to = ms; /* execTransaction -> MultiSend */
            }
        }

        tb_safe_proposal_t prop;
        if (tb_safe_propose_exec(c->http, &c->cfg.safe, &exec_to,
                                 0, calldata, calldata_len, now_ms, &prop,
                                 err) != 0)
            return -1;
        tb_atomic_add_u64(&c->stats.proposals, 1);
        if (c->cfg.on_propose)
            c->cfg.on_propose(&prop, c->cfg.propose_user);

        /* прежнее живое trade-предложение заменено (nonce тот же) */
        if (lp->active && lp->tx_id) {
            for (size_t i = 0; i < TB_DEX_MAX_SESSIONS; i++)
                if (c->txs[i].tx_id == lp->tx_id && c->txs[i].live)
                    c->txs[i].superseded = true;
            tb_atomic_add_u64(&c->stats.supersedes, 1);
        }

        memset(s, 0, sizeof(*s));
        s->tx_id = ++c->next_tx;
        s->live = true;
        s->safe_nonce = prop.safe_nonce;
        s->expires_ms = prop.expires_at_ms;
        memcpy(s->safe_tx_hash, prop.safe_tx_hash, 32);
        { /* блок рождения предложения: getLogs пойдёт ОТ него, а не от
           * «текущий-4096» (ревью L1); ошибка -> 0 = fallback окно */
            tb_exec_dex_venue_t v0 = { 0 };
            snprintf(v0.name, sizeof(v0.name), "safe-rpc");
            snprintf(v0.rpc_url, sizeof(v0.rpc_url), "%s",
                     c->cfg.safe.rpc_url);
            if (c->cfg.safe.rpc_host_pinned[0])
                snprintf(v0.rpc_host_pinned,
                         sizeof(v0.rpc_host_pinned), "%s",
                         c->cfg.safe.rpc_host_pinned);
            tb_error_t be = TB_ERROR_INIT;
            (void)tb_exec_dex_block_number(c->http, &v0, &s->born_block,
                                           &be);
        }
        s->out_token = out_tok;
        s->balance_before = before;
        s->amount_out = out; /* оценка; истина придёт дельтой в poll */

        lp->active = true;
        lp->approve = false;
        lp->safe_nonce = prop.safe_nonce;
        lp->expires_ms = prop.expires_at_ms;
        lp->tx_id = s->tx_id;

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
        /* Заменено новым предложением того же nonce — отказ старого */
        if (s->superseded) {
            s->tx_id = 0;
            TB_ERROR_FAIL(err, TB_DOMAIN_USER, TB_EXEC_BACKEND,
                          "dex: предложение заменено более свежим "
                          "(тот же nonce)");
        }
        /* Предложение ждёт человека: следим за nonce Safe. Истекло окно
         * — отказ: на цепи ничего не было, газ не потрачен. */
        if (now_ms > s->expires_ms) {
            s->tx_id = 0;
            tb_atomic_add_u64(&c->stats.proposals_expired, 1);
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

        /* атрибуция (H1.5, правка ревью): событие ExecutionSuccess/
         * Failure нашего safeTxHash доказывает исполнение ИМЕННО
         * нашего предложения. Три состояния не смешиваются:
         * FOUND / ABSENT (запрос успешен, события нет) / ERROR (RPC
         * не ответил). Колбэк вызывается ВСЕГДА — иначе журнал теряет
         * даже факт неудавшейся проверки */
        {
            tb_exec_dex_execution_t ex = { 0 };
            ex.balance_delta = *amount_out;
            memcpy(ex.safe_tx_hash, s->safe_tx_hash, 32);
            tb_error_t serr2 = TB_ERROR_INIT;
            bool found = false, success = false;
            uint64_t blk2 = 0;
            /* getLogs от блока рождения предложения (ревью L1),
             * fallback — окно 4096 от текущего, если блок не взяли */
            uint64_t from = s->born_block;
            if (!from) {
                tb_exec_dex_venue_t v = { 0 };
                snprintf(v.name, sizeof(v.name), "safe-rpc");
                snprintf(v.rpc_url, sizeof(v.rpc_url), "%s",
                         c->cfg.safe.rpc_url);
                if (c->cfg.safe.rpc_host_pinned[0])
                    snprintf(v.rpc_host_pinned,
                             sizeof(v.rpc_host_pinned), "%s",
                             c->cfg.safe.rpc_host_pinned);
                tb_error_t blkh = TB_ERROR_INIT;
                uint64_t cur = 0;
                (void)tb_exec_dex_block_number(c->http, &v, &cur, &blkh);
                from = cur > 4096 ? cur - 4096 : 0;
            }
            if (tb_safe_execution_status(c->http, &c->cfg.safe,
                                         s->safe_tx_hash, from, &found,
                                         &success, &blk2, &serr2) == 0) {
                ex.status = found ? TB_ATTRIB_FOUND : TB_ATTRIB_ABSENT;
                ex.success = success;
                ex.block = blk2;
            } else {
                ex.status = TB_ATTRIB_ERROR; /* не «нет события», а
                                              * «не смогли проверить» */
            }
            if (c->cfg.on_execution)
                c->cfg.on_execution(&ex, c->cfg.execution_user);
        }

        s->tx_id = 0;
        /* исполнение = рост nonce: живое предложение этого ctx закрыто */
        c->live.active = false;
        c->live.approve = false;
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

int tb_exec_dex_find_pool(tb_http_t *http, const tb_exec_dex_venue_t *v,
                          const tb_addr_t *token_a, const tb_addr_t *token_b,
                          uint32_t fee, bool *found, tb_addr_t *pool,
                          tb_error_t *err)
{
    if (tb_addr_is_native(&v->factory)) {
        TB_ERROR_FAIL(err, TB_DOMAIN_USER, TB_EXEC_BACKEND,
                      "find: у биржи %s нет factory", v->name);
    }

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

    /* ответ — слово; адрес = последние 20 байт (40 hex-символов);
     * "0x" — фабрика не знает такой пары/fee */
    const char *fn = is_v2 ? "getPair" : "getPool";
    size_t len = strlen(result);
    if (len <= 2) {
        *found = false;
        return 0;
    }
    if (len < 42 || (len - 2) % 2 != 0) {
        TB_ERROR_SET(err, TB_DOMAIN_USER, TB_EXEC_BACKEND,
                     "%s %s: ответ не слово '%.32s'", fn, v->name, result);
        return -1;
    }
    char addr_hex[43] = "0x";
    memcpy(addr_hex + 2, result + len - 40, 40);
    if (tb_addr_from_hex(pool, addr_hex, err) != 0) {
        TB_ERROR_WRAP(err, "%s %s", fn, v->name);
        return -1;
    }
    *found = true;
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

    bool found = false;
    tb_addr_t on_chain;
    if (tb_exec_dex_find_pool(http, v, token_a, token_b, fee, &found,
                              &on_chain, err) != 0)
        return -1;

    char expect_hex[43], got_hex[43];
    tb_addr_to_hex(expect_pool, expect_hex, sizeof(expect_hex));
    tb_addr_to_hex(&on_chain, got_hex, sizeof(got_hex));
    const bool is_v2 = v->kind == TB_DEX_V2;
    if (!found) {
        TB_ERROR_SET(err, TB_DOMAIN_USER, TB_EXEC_BACKEND,
                     "%s %s: пула %s нет (фабрика вернула 0x0; "
                     "пара/fee в конфиге не совпадают с цепью)",
                     is_v2 ? "getPair" : "getPool", v->name, expect_hex);
        return -1;
    }
    if (!tb_addr_eq(&on_chain, expect_pool)) {
        TB_ERROR_SET(err, TB_DOMAIN_USER, TB_EXEC_BACKEND,
                     "%s %s: конфиг 0x%s, фабрика 0x%s",
                     is_v2 ? "getPair" : "getPool", v->name, expect_hex + 2,
                     got_hex + 2);
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
