#define TB_CURRENT_GROUP TB_G_DEX /* зона ответственности файла */

/* ==========================================================================
 *  tb/exec/safe_exec.c — живое исполнение из Safe.
 *
 *  Цепочка submit: chainId -> nonce(Safe) -> цены газа -> estimate ->
 *  потолок газа -> EIP-712 SafeTx-хеш -> подпись (v=parity+27) ->
 *  execTransaction calldata -> EIP-1559 -> eth_sendRawTransaction.
 *  Poll: eth_getTransactionReceipt; фактический выход исполнитель
 *  берёт дельтой balanceOf(Safe) — истина с цепи, не наша котировка.
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

int tb_safe_sign(const tb_safe_cfg_t *cfg, const uint8_t safe_tx_hash[32],
                 uint8_t sig65[65], tb_error_t *err)
{
    /* Safe ECDSA-владелец: подпись личного сообщения EIP-191
     * keccak("\x19Ethereum Signed Message:\n32" || SafeTxHash), v=27/28 */
    uint8_t pref[28 + 32], shash[32];
    memcpy(pref, "\x19"
                 "Ethereum Signed Message:\n32",
           28);
    memcpy(pref + 28, safe_tx_hash, 32);
    tb_keccak256(pref, sizeof(pref), shash);

    int parity = 0;
    tb_error_t serr = TB_ERROR_INIT;
    if (tb_ecdsa_sign(&cfg->key, shash, sig65, sig65 + 32, &parity,
                      &serr) != 0) {
        if (err)
            *err = serr;
        return -1;
    }
    sig65[64] = (uint8_t)(parity + 27);
    return 0;
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
    char data_hex[4 + 64 * 2 + 3];
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

int tb_safe_receipt(tb_http_t *h, const tb_safe_cfg_t *c,
                    const uint8_t tx_hash[32], bool *is_null, int *status,
                    tb_error_t *err)
{
    char hash_hex[67];
    strcpy(hash_hex, "0x");
    for (int i = 0; i < 32; i++)
        sprintf(hash_hex + 2 + i * 2, "%02x", tx_hash[i]);
    char params[128];
    snprintf(params, sizeof(params), "[\"%s\"]", hash_hex);

    char res[512];
    rpc_resp_owner_t owner = {0};
    if (rpc_call(h, c, "eth_getTransactionReceipt", params, res, sizeof(res),
                 &owner, err) != 0)
        return -1;
    /* статус ищем в СЫРОМ теле (объект receipt может быть длинным);
     * st указывает внутрь owner.resp — используем ДО free (иначе UAF) */
    const char *st = strstr(owner.body, "\"status\":\"0x");

    if (strcmp(res, "null") == 0 || strstr(res, "null") == res) {
        free(owner.resp);
        *is_null = true;
        return 0;
    }
    *is_null = false;
    if (!st) {
        free(owner.resp);
        TB_ERROR_FAIL(err, TB_DOMAIN_USER, TB_EXEC_BACKEND,
                      "receipt: нет статуса");
    }
    *status = (int)strtol(st + 12, NULL, 16);
    free(owner.resp);
    return 0;
}

int tb_safe_send_exec(tb_http_t *h, const tb_safe_cfg_t *c, const tb_addr_t *to,
                      uint64_t value, const uint8_t *data, size_t data_len,
                      uint8_t tx_hash[32], tb_error_t *err)
{
    uint64_t chain = c->chain_id;
    if (!chain && tb_safe_chain_id(h, c, &chain, err) != 0)
        return -1;
    tb_safe_cfg_t cfg = *c;
    cfg.chain_id = chain;

    uint64_t safe_nonce = 0;
    if (tb_safe_nonce(h, &cfg, &safe_nonce, err) != 0)
        return -1;

    uint64_t gas_price = 0, prio = 0;
    if (rpc_u64_hex(h, &cfg, "eth_gasPrice", "[]", &gas_price, err) != 0)
        return -1;
    if (rpc_u64_hex(h, &cfg, "eth_maxPriorityFeePerGas", "[]", &prio, err)
        != 0) {
        /* не все узлы дают maxPriority — берём 1.5 gwei запас */
        prio = 1500000000ull;
        tb_error_clear(err);
    }
    if (cfg.max_fee_wei && gas_price + prio > cfg.max_fee_wei)
        TB_ERROR_FAIL(err, TB_DOMAIN_USER, TB_EXEC_BACKEND,
                      "газ %llu+%llu выше потолка %llu",
                      (unsigned long long)gas_price,
                      (unsigned long long)prio,
                      (unsigned long long)cfg.max_fee_wei);

    /* estimate: execTransaction с нулевой подписью (симуляция) */
    static uint8_t exec_buf[1024 + 65536];
    uint8_t zero_sig[65] = { 0 };
    size_t exec_len = tb_safe_build_exec_tx(exec_buf, sizeof(exec_buf), to,
                                            value, data, data_len, zero_sig);
    if (!exec_len)
        TB_ERROR_FAIL(err, TB_DOMAIN_CORE, TB_CORE_OVERFLOW, "exec: calldata");

    static char data_hex[4 + 2 * (1024 + 65536) + 3];
    static char params[140000];
    char from_hex[43], to_hex[43];
    tb_addr_to_hex(&cfg.key.address, from_hex, sizeof(from_hex));
    tb_addr_to_hex(&cfg.safe, to_hex, sizeof(to_hex));
    {
        char *p = data_hex;
        p += sprintf(p, "0x");
        for (size_t i = 0; i < exec_len; i++)
            p += sprintf(p, "%02x", exec_buf[i]);
        snprintf(params, sizeof(params),
                 "[{\"from\":\"%s\",\"to\":\"%s\",\"data\":\"%s\"}]",
                 from_hex, to_hex, data_hex);
    }
    uint64_t gas_est = 0;
    if (rpc_u64_hex(h, &cfg, "eth_estimateGas", params, &gas_est, err) != 0)
        return -1;

    uint64_t gas =
        cfg.gas_limit ? cfg.gas_limit : gas_est + gas_est / 3 + 30000;

    /* SafeTx-хеш и подпись владельца-бота */
    uint8_t stx[32], sig[65];
    tb_safe_tx_hash(&cfg, to, value, data, data_len, safe_nonce, stx, err);
    if (tb_safe_sign(&cfg, stx, sig, err) != 0)
        return -1;

    exec_len = tb_safe_build_exec_tx(exec_buf, sizeof(exec_buf), to, value,
                                     data, data_len, sig);
    if (!exec_len)
        TB_ERROR_FAIL(err, TB_DOMAIN_CORE, TB_CORE_OVERFLOW,
                      "exec: calldata+sig");

    tb_tx1559_t tx = {
        .chain_id = chain,
        .nonce = 0,
        .max_priority_fee = prio,
        .max_fee = gas_price + prio,
        .gas_limit = gas,
        .to = cfg.safe, /* execTransaction вызывается НА контракте Safe */
        .value = 0,
        .data = exec_buf,
        .data_len = exec_len,
    };

    char acct_params[128];
    snprintf(acct_params, sizeof(acct_params), "[\"%s\",\"pending\"]",
             from_hex);
    if (rpc_u64_hex(h, &cfg, "eth_getTransactionCount", acct_params,
                    &tx.nonce, err) != 0)
        return -1;

    static char raw[4 + 2 * (1024 + 65536) + 3];
    if (tb_tx1559_sign(&cfg.key, &tx, raw, sizeof(raw), tx_hash, NULL,
                       err) != 0)
        return -1;

    static char send_params[140000];
    snprintf(send_params, sizeof(send_params), "[\"%s\"]", raw);
    char res[80];
    rpc_resp_owner_t owner = {0};
    if (rpc_call(h, &cfg, "eth_sendRawTransaction", send_params, res,
                 sizeof(res), &owner, err) != 0)
        return -1;
    free(owner.resp);

    if (strlen(res) != 66)
        TB_ERROR_FAIL(err, TB_DOMAIN_USER, TB_EXEC_BACKEND,
                      "sendRaw: хеш невалиден: '%.16s'", res);
    for (int i = 0; i < 32; i++)
        sscanf(res + 2 + i * 2, "%2hhx", &tx_hash[i]);
    return 0;
}
