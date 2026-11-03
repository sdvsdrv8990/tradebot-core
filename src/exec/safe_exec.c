#define TB_CURRENT_GROUP TB_G_DEX /* зона ответственности файла */

/* ==========================================================================
 *  tb/exec/safe_exec.c — подготовка исполнения из Safe (безключевой).
 *
 *  Бот готовит: chainId -> nonce(Safe) -> EIP-712 SafeTx-хеш ->
 *  предложение наверх (артефакт + очередь Safe UI через Transaction
 *  Service). Приватных ключей и отправки нет как класса: подписывает и
 *  исполняет человек (Safe 1-из-1). Результат бот читает дельтой
 *  balanceOf после роста nonce.
 * ========================================================================== */
#include "tb/exec/safe_exec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tb/core/keccak.h"
#include "tb/core/time.h"

/* ---- ABI-слова ---- */

static void word_from_u64(uint8_t w[32], uint64_t v)
{
    memset(w, 0, 32);
    for (int i = 0; i < 8; i++)
        w[31 - i] = (uint8_t)(v >> (8 * i));
}

static void word_from_addr(uint8_t w[32], const tb_addr_t *a)
{
    memset(w, 0, 32);
    memcpy(w + 12, a->b, 20);
}

static void word_from_u256(uint8_t w[32], const tb_u256_t *v)
{
    memset(w, 0, 32);
    for (int i = 0; i < 4; i++)
        for (int b = 0; b < 8; b++)
            w[31 - (i * 8 + b)] = (uint8_t)(v->w[i] >> (8 * b));
}

static void put32(uint8_t *p, size_t *n, const uint8_t w[32])
{
    memcpy(p + *n, w, 32);
    *n += 32;
}

static void put_word_u64(uint8_t *p, size_t *n, uint64_t v)
{
    uint8_t w[32];
    word_from_u64(w, v);
    put32(p, n, w);
}

static void put_selector(uint8_t *p, size_t *n, uint32_t sel)
{
    p[(*n)++] = (uint8_t)(sel >> 24);
    p[(*n)++] = (uint8_t)(sel >> 16);
    p[(*n)++] = (uint8_t)(sel >> 8);
    p[(*n)++] = (uint8_t)sel;
}

/* ---- Селекторы (выверены test_keccak) ---- */

#define SEL_EXACT_INPUT 0xb858183fu
#define SEL_EXEC_TX     0x6a761202u
#define SEL_BALANCE_OF  0x70a08231u
#define SEL_ALLOWANCE   0xdd62ed3eu
#define SEL_APPROVE     0x095ea7b3u
#define SEL_SAFE_NONCE  0x1d6da664u /* nonce() */

/* ---- Calldata ---- */

size_t tb_safe_build_exact_input(uint8_t *buf, size_t cap,
                                 const tb_route_t *route,
                                 const tb_addr_t *safe,
                                 const tb_u256_t *amount_in,
                                 const tb_u256_t *min_out)
{
    /* exactInput((bytes,address,uint256,uint256)): голова — offset к
     * bytes (160), recipient, amountIn, minOut; хвост — путь V3. */
    size_t padded = ((route->len + 31) / 32) * 32;
    size_t need = 4 + 32 * 4 + 32 + padded;
    if (cap < need || route->len == 0)
        return 0;

    size_t n = 0;
    put_selector(buf, &n, SEL_EXACT_INPUT);
    put_word_u64(buf, &n, 160); /* offset к path */
    uint8_t w[32];
    word_from_addr(w, safe);
    put32(buf, &n, w);
    word_from_u256(w, amount_in);
    put32(buf, &n, w);
    word_from_u256(w, min_out);
    put32(buf, &n, w);
    put_word_u64(buf, &n, route->len);
    memset(buf + n, 0, padded);
    memcpy(buf + n, route->bytes, route->len);
    n += padded;
    return n;
}

size_t tb_safe_build_approve(uint8_t *buf, size_t cap,
                             const tb_addr_t *spender,
                             const tb_u256_t *amount)
{
    /* approve(address,uint256): селектор + адрес + сумма */
    if (cap < 4 + 64)
        return 0;
    size_t n = 0;
    put_selector(buf, &n, SEL_APPROVE);
    uint8_t w[32];
    word_from_addr(w, spender);
    put32(buf, &n, w);
    word_from_u256(w, amount);
    put32(buf, &n, w);
    return n;
}

size_t tb_safe_build_exec_tx(uint8_t *buf, size_t cap, const tb_addr_t *to,
                             uint64_t value, const uint8_t *data,
                             size_t data_len, const uint8_t sig65[65])
{
    /* execTransaction(to, value, data, operation, safeTxGas, baseGas,
     * gasPrice, gasToken, refundReceiver, signatures) */
    size_t data_padded = ((data_len + 31) / 32) * 32;
    size_t need = 4 + 32 * 10 + 32 + data_padded + 32 + 96;
    if (cap < need)
        return 0;

    size_t head_fixed = 4 + 32 * 10;
    size_t data_off = head_fixed;
    size_t sig_off = data_off + 32 + data_padded;

    size_t n = 0;
    put_selector(buf, &n, SEL_EXEC_TX);
    uint8_t w[32];

    word_from_addr(w, to);
    put32(buf, &n, w);
    put_word_u64(buf, &n, value);
    put_word_u64(buf, &n, data_off);
    put_word_u64(buf, &n, 0); /* operation = CALL */
    put_word_u64(buf, &n, 0); /* safeTxGas */
    put_word_u64(buf, &n, 0); /* baseGas */
    put_word_u64(buf, &n, 0); /* gasPrice */
    memset(w, 0, 32);
    put32(buf, &n, w); /* gasToken = 0 */
    put32(buf, &n, w); /* refundReceiver = 0 */
    put_word_u64(buf, &n, sig_off);

    put_word_u64(buf, &n, data_len);
    memset(buf + n, 0, data_padded);
    if (data && data_len)
        memcpy(buf + n, data, data_len);
    n += data_padded;

    put_word_u64(buf, &n, 65);
    memset(buf + n, 0, 96);
    memcpy(buf + n, sig65, 65);
    n += 96;
    return n;
}

/* ---- EIP-712 SafeTx ---- */

int tb_safe_tx_hash(const tb_safe_cfg_t *cfg, const tb_addr_t *to,
                    uint64_t value, const uint8_t *data, size_t data_len,
                    uint64_t safe_nonce, uint8_t hash[32], tb_error_t *err)
{
    const char *tx_type =
        "SafeTx(address to,uint256 value,bytes data,uint8 operation,"
        "uint256 safeTxGas,uint256 baseGas,uint256 gasPrice,"
        "address gasToken,address refundReceiver,uint256 nonce)";
    const char *dom_type =
        "EIP712Domain(uint256 chainId,address verifyingContract)";

    uint8_t th[32], data_hash[32], dom_sep[32];
    tb_keccak256(tx_type, strlen(tx_type), th);
    tb_keccak256(data, data_len, data_hash);
    {
        uint8_t db[128];
        size_t dn = 0;
        uint8_t dh[32];
        tb_keccak256(dom_type, strlen(dom_type), dh);
        put32(db, &dn, dh);
        put_word_u64(db, &dn, cfg->chain_id);
        uint8_t w[32];
        word_from_addr(w, &cfg->safe);
        put32(db, &dn, w);
        tb_keccak256(db, dn, dom_sep);
    }

    uint8_t buf[512];
    size_t n = 0;
    put32(buf, &n, th);
    uint8_t w[32];
    word_from_addr(w, to);
    put32(buf, &n, w);
    put_word_u64(buf, &n, value);
    put32(buf, &n, data_hash);
    put_word_u64(buf, &n, 0); /* operation */
    put_word_u64(buf, &n, 0); /* safeTxGas */
    put_word_u64(buf, &n, 0); /* baseGas */
    put_word_u64(buf, &n, 0); /* gasPrice */
    memset(w, 0, 32);
    put32(buf, &n, w); /* gasToken */
    put32(buf, &n, w); /* refundReceiver */
    put_word_u64(buf, &n, safe_nonce);
    tb_keccak256(buf, n, hash);
    return 0;
}


/* ---- Предложения (propose-only) ---- */

int tb_safe_propose_exec(tb_http_t *h, const tb_safe_cfg_t *c,
                         const tb_addr_t *to, uint64_t value,
                         const uint8_t *data, size_t data_len,
                         uint64_t now_ms, tb_safe_proposal_t *out,
                         tb_error_t *err)
{
    if (!out || !data || data_len == 0 || data_len > sizeof(out->data))
        TB_ERROR_FAIL(err, TB_DOMAIN_CORE, TB_CORE_INVALID_ARG,
                      "propose: аргументы");

    tb_safe_cfg_t cfg = *c;
    memset(out, 0, sizeof(*out));
    uint64_t chain = c->chain_id;
    if (!chain && tb_safe_chain_id(h, &cfg, &chain, err) != 0)
        return -1;
    cfg.chain_id = chain;

    uint64_t nonce = 0;
    if (tb_safe_nonce(h, &cfg, &nonce, err) != 0)
        return -1;

    if (tb_safe_tx_hash(&cfg, to, value, data, data_len, nonce,
                        out->safe_tx_hash, err) != 0)
        return -1;

    out->chain_id = chain;
    out->safe = c->safe;
    out->to = *to;
    out->value = value;
    memcpy(out->data, data, data_len);
    out->data_len = data_len;
    out->safe_nonce = nonce;
    snprintf(out->kind, sizeof(out->kind), "trade");
    out->created_ms = now_ms;
    out->expires_at_ms = now_ms + TB_SAFE_PROPOSAL_WATCH_MS;
    return 0;
}

size_t tb_safe_proposal_exec_calldata(const tb_safe_proposal_t *p,
                                      uint8_t *buf, size_t cap)
{
    /* подписей бота больше нет: кошелёк человека подписывает сам;
     * calldata здесь — справочный «сырой» execTransaction для
     * инструментов, работающих с одной сигнатурой владельца */
    uint8_t no_sig[65] = { 0 };
    return tb_safe_build_exec_tx(buf, cap, &p->to, p->value, p->data,
                                 p->data_len, no_sig);
}

/* Десятичное u256-слово calldata по смещению байта (для verify-блока). */
static void word_dec(const uint8_t *data, size_t off, char *buf, size_t cap)
{
    tb_u256_t v = tb_u256_from_u64(0);
    for (int w = 0; w < 4; w++) {
        uint64_t acc = 0;
        for (int b = 0; b < 8; b++)
            acc = (acc << 8) | data[off + w * 8 + b];
        v.w[3 - w] = acc;
    }
    tb_u256_to_dec(&v, buf, cap);
}

size_t tb_safe_proposal_json(const tb_safe_proposal_t *p, char *out,
                             size_t cap)
{
    if (!p || !out || cap == 0)
        return 0;

    char safe_h[43], to_h[43], stx_h[67], data_h[2 + 512 * 2 + 1];
    tb_addr_to_hex(&p->safe, safe_h, sizeof(safe_h));
    tb_addr_to_hex(&p->to, to_h, sizeof(to_h));
    stx_h[0] = '0';
    stx_h[1] = 'x';
    for (int i = 0; i < 32; i++)
        sprintf(stx_h + 2 + i * 2, "%02x", p->safe_tx_hash[i]);
    data_h[0] = '0';
    data_h[1] = 'x';
    for (size_t i = 0; i < p->data_len; i++)
        sprintf(data_h + 2 + i * 2, "%02x", p->data[i]);
    data_h[2 + p->data_len * 2] = '\0';

    /* verify-блок: числа для сверки с кошельком (не hex) — точный
     * calldata exactInput несёт amountIn/minOut в словах 3 и 4 после
     * селектора (голова: offset, recipient, amountIn, minOut) */
    char vin[80] = "\"\"", vmin[80] = "\"\"";
    if (p->data_len >= 4 + 32 * 4 && (p->data[0] << 24 | p->data[1] << 16
                                       | p->data[2] << 8 | p->data[3])
                                          == (int)SEL_EXACT_INPUT) {
        word_dec(p->data, 4 + 32 * 2, vin, sizeof(vin));
        word_dec(p->data, 4 + 32 * 3, vmin, sizeof(vmin));
    }

    int n = snprintf(out, cap,
                     "{\"kind\":\"%s\",\"chain_id\":%llu,"
                     "\"safe\":\"%s\",\"to\":\"%s\",\"value\":%llu,"
                     "\"data\":\"%s\",\"operation\":0,\"safe_tx_gas\":0,"
                     "\"base_gas\":0,\"gas_price\":\"0\",\"nonce\":%llu,"
                     "\"safe_tx_hash\":\"%s\","
                     "\"verify\":{\"amount_in_wei\":\"%s\","
                     "\"amount_out_minimum_wei\":\"%s\"},"
                     "\"created_ms\":%llu,\"expires_at_ms\":%llu}",
                     p->kind, (unsigned long long)p->chain_id, safe_h, to_h,
                     (unsigned long long)p->value, data_h,
                     (unsigned long long)p->safe_nonce, stx_h, vin, vmin,
                     (unsigned long long)p->created_ms,
                     (unsigned long long)p->expires_at_ms);
    return n < 0 ? 0 : (size_t)n;
}

/* ---- RPC ---- */

/* Вызвать метод; body_out (опционально) получает сырой ответ целиком. */
static int rpc_post(tb_http_t *h, const tb_safe_cfg_t *c, const char *method,
                    const char *params, const char **body_out,
                    tb_error_t *err)
{
    /* большой буфер: estimate/sendRaw несут весь calldata в params */
    static char body[200000];
    int n = snprintf(body, sizeof(body),
                     "{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"params\":%s,"
                     "\"id\":1}",
                     method, params);
    if (n < 0 || (size_t)n >= sizeof(body))
        TB_ERROR_FAIL(err, TB_DOMAIN_USER, TB_EXEC_BACKEND, "rpc: запрос");

    tb_http_req_t req = {0};
    strcpy(req.method, "POST");
    strcpy(req.url, c->rpc_url);
    if (c->rpc_host_pinned[0])
        strcpy(req.host_pinned, c->rpc_host_pinned);
    req.body = body;
    req.body_len = (size_t)n;
    req.timeout_ms = c->timeout_ms ? c->timeout_ms : 10000;

    tb_http_resp_t *resp = calloc(1, sizeof(*resp));
    if (!resp)
        TB_ERROR_FAIL(err, TB_DOMAIN_CORE, TB_CORE_NO_MEMORY, "rpc: память");
    int rc = -1;
    if (tb_http_request(h, &req, resp, err) != 0) {
        TB_ERROR_PROMOTE(err, TB_DOMAIN_USER, TB_EXEC_BACKEND, "rpc: транспорт");
        goto out;
    }
    if (resp->status != 200) {
        TB_ERROR_SET(err, TB_DOMAIN_USER, TB_EXEC_BACKEND,
                     "rpc %s: HTTP %ld", method, (long)resp->status);
        goto out;
    }
    if (strstr(resp->body, "\"error\"")) {
        TB_ERROR_SET(err, TB_DOMAIN_USER, TB_EXEC_BACKEND,
                     "rpc %s: ошибка узла: %.96s", method, resp->body);
        goto out;
    }
    if (body_out)
        *body_out = resp->body; /* живёт до конца функции */
    rc = 0;
out:
    if (rc != 0) {
        free(resp);
        if (body_out)
            *body_out = NULL;
    }
    return rc;
}

typedef struct rpc_resp_owner {
    tb_http_resp_t *resp;
    const char *body;
} rpc_resp_owner_t;

static int rpc_call(tb_http_t *h, const tb_safe_cfg_t *c, const char *method,
                    const char *params, char *result, size_t res_cap,
                    rpc_resp_owner_t *owner, tb_error_t *err)
{
    const char *body = NULL;
    if (rpc_post(h, c, method, params, &body, err) != 0)
        return -1;
    owner->resp = (tb_http_resp_t *)((char *)body - offsetof(
                                         tb_http_resp_t, body));
    owner->body = body;

    if (result) {
        const char *p = strstr(body, "\"result\":");
        if (!p) {
            TB_ERROR_SET(err, TB_DOMAIN_USER, TB_EXEC_BACKEND,
                         "rpc %s: нет result", method);
            return -1;
        }
        p += 9;
        while (*p == ' ' || *p == '\"')
            p++;
        size_t i = 0;
        while (p[i] && p[i] != '\"' && p[i] != '}' && p[i] != ','
               && i + 1 < res_cap)
            i++;
        memcpy(result, p, i);
        result[i] = '\0';
    }
    return 0;
}

static int rpc_u64_hex(tb_http_t *h, const tb_safe_cfg_t *c, const char *method,
                       const char *params, uint64_t *out, tb_error_t *err)
{
    char res[80];
    rpc_resp_owner_t owner = {0};
    if (rpc_call(h, c, method, params, res, sizeof(res), &owner, err) != 0)
        return -1;
    free(owner.resp);
    if (res[0] != '0' || res[1] != 'x')
        TB_ERROR_FAIL(err, TB_DOMAIN_USER, TB_EXEC_BACKEND,
                      "rpc %s: не hex: '%.16s'", method, res);
    *out = strtoull(res + 2, NULL, 16);
    return 0;
}

int tb_safe_chain_id(tb_http_t *h, const tb_safe_cfg_t *c, uint64_t *out,
                     tb_error_t *err)
{
    return rpc_u64_hex(h, c, "eth_chainId", "[]", out, err);
}

static int call_addr_method(tb_http_t *h, const tb_safe_cfg_t *c,
                            uint32_t selector, const tb_addr_t *to,
                            const tb_addr_t *arg1, const tb_addr_t *arg2,
                            tb_u256_t *out, tb_error_t *err)
{
    uint8_t data[4 + 64];
    size_t n = 0;
    put_selector(data, &n, selector);
    uint8_t w[32];
    word_from_addr(w, arg1);
    memcpy(data + n, w, 32);
    word_from_addr(w, arg2 ? arg2 : arg1);
    memcpy(data + n + 32, w, 32);

    char to_hex[43];
    tb_addr_to_hex(to, to_hex, sizeof(to_hex));
    char data_hex[2 + sizeof(data) * 2 + 1]; /* "0x" + hex всех байтов */
    char *p = data_hex;
    p += sprintf(p, "0x");
    for (size_t i = 0; i < sizeof(data); i++)
        p += sprintf(p, "%02x", data[i]);

    char params[512];
    snprintf(params, sizeof(params),
             "[{\"to\":\"%s\",\"data\":\"%s\"},\"latest\"]", to_hex,
             data_hex);

    char res[80];
    rpc_resp_owner_t owner = {0};
    if (rpc_call(h, c, "eth_call", params, res, sizeof(res), &owner, err)
        != 0)
        return -1;
    free(owner.resp);

    if (strlen(res) < 66)
        TB_ERROR_FAIL(err, TB_DOMAIN_USER, TB_EXEC_BACKEND,
                      "eth_call: пустой результат (%.16s)", res);
    memset(out, 0, sizeof(*out));
    for (int word = 0; word < 4; word++) {
        uint64_t v = 0;
        for (int i = 0; i < 16; i++) {
            char ch = res[2 + word * 16 + i];
            unsigned d = 0;
            if (ch >= '0' && ch <= '9')
                d = (unsigned)(ch - '0');
            else if (ch >= 'a' && ch <= 'f')
                d = (unsigned)(ch - 'a' + 10);
            v = (v << 4) | d;
        }
        out->w[3 - word] = v;
    }
    return 0;
}

int tb_safe_nonce(tb_http_t *h, const tb_safe_cfg_t *c, uint64_t *out,
                  tb_error_t *err)
{
    tb_u256_t v;
    if (call_addr_method(h, c, SEL_SAFE_NONCE, &c->safe, &c->safe, NULL, &v,
                         err) != 0)
        return -1;
    if (v.w[1] || v.w[2] || v.w[3])
        TB_ERROR_FAIL(err, TB_DOMAIN_USER, TB_EXEC_BACKEND,
                      "нонс Safe вне u64");
    *out = v.w[0];
    return 0;
}

int tb_safe_balance_of(tb_http_t *h, const tb_safe_cfg_t *c,
                       const tb_addr_t *token, const tb_addr_t *holder,
                       tb_u256_t *out, tb_error_t *err)
{
    return call_addr_method(h, c, SEL_BALANCE_OF, token, holder, NULL, out,
                            err);
}

int tb_safe_allowance(tb_http_t *h, const tb_safe_cfg_t *c,
                      const tb_addr_t *token, const tb_addr_t *owner,
                      const tb_addr_t *spender, tb_u256_t *out,
                      tb_error_t *err)
{
    return call_addr_method(h, c, SEL_ALLOWANCE, token, owner, spender, out,
                            err);
}
