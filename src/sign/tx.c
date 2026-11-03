#define TB_CURRENT_GROUP TB_G_CORE /* зона ответственности файла */

/* ==========================================================================
 *  tb/sign/tx.c — secp256k1 + RLP + EIP-1559.
 *
 *  Схема подписи: keccak256(rlp([chainId, nonce, maxPrio, maxFee, gas,
 *  to, value, data, []])) -> ECDSA low-S -> y-parity из recoverable.
 *  Итог: rlp([…, [], v=parity, r, s]).
 * ========================================================================== */
#include "tb/sign/tx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tb/core/keccak.h"
#include <secp256k1.h>
#include <secp256k1_recovery.h>

static secp256k1_context *sign_ctx(void)
{
    static secp256k1_context *ctx;
    if (!ctx)
        ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN
                                       | SECP256K1_CONTEXT_VERIFY);
    return ctx;
}

/* ---- Ключ ---- */

int tb_key_from_hex(tb_key_t *k, const char *hex32, tb_error_t *err)
{
    memset(k, 0, sizeof(*k));
    if (!hex32)
        TB_ERROR_FAIL(err, TB_DOMAIN_CORE, TB_CORE_INVALID_ARG,
                      "ключ: пустая строка");
    if (hex32[0] == '0' && (hex32[1] == 'x' || hex32[1] == 'X'))
        hex32 += 2;
    if (strlen(hex32) != 64)
        TB_ERROR_FAIL(err, TB_DOMAIN_CORE, TB_CORE_INVALID_ARG,
                      "ключ: нужно 64 hex-символа");

    for (int i = 0; i < 32; i++) {
        unsigned v;
        if (sscanf(hex32 + i * 2, "%2x", &v) != 1)
            TB_ERROR_FAIL(err, TB_DOMAIN_CORE, TB_CORE_INVALID_ARG,
                          "ключ: не-hex символ");
        k->secret[i] = (uint8_t)v;
    }

    secp256k1_pubkey pub;
    if (!secp256k1_ec_pubkey_create(sign_ctx(), &pub, k->secret))
        TB_ERROR_FAIL(err, TB_DOMAIN_CORE, TB_CORE_INVALID_ARG,
                      "ключ: вне диапазона (нулевой/>=n)");
    size_t outlen = 65;
    uint8_t uncomp[65];
    secp256k1_ec_pubkey_serialize(sign_ctx(), uncomp, &outlen, &pub,
                                   SECP256K1_EC_UNCOMPRESSED);
    memcpy(k->pub, uncomp + 1, 64);

    uint8_t h[32];
    tb_keccak256(k->pub, 64, h);
    memcpy(k->address.b, h + 12, 20);
    k->ok = true;
    return 0;
}

int tb_key_from_env(tb_key_t *k, const char *var, tb_error_t *err)
{
    const char *v = getenv(var);
    if (!v || !*v)
        TB_ERROR_FAIL(err, TB_DOMAIN_CORE, TB_CORE_NOT_INIT,
                      "ключ: переменная %s не задана", var);
    return tb_key_from_hex(k, v, err);
}

/* ---- RLP ---- */

void tb_rlp_init(tb_rlp_t *r, uint8_t *buf, size_t cap)
{
    r->buf = buf;
    r->cap = cap;
    r->len = 0;
    r->overflow = false;
}

static void rlp_put(tb_rlp_t *r, const uint8_t *p, size_t n)
{
    if (r->len + n > r->cap) {
        r->overflow = true;
        return;
    }
    if (n)
        memcpy(r->buf + r->len, p, n); /* n==0 с p==NULL — UB по
                                        * стандарту (UBSan прав) */
    r->len += n;
}

static void rlp_head(tb_rlp_t *r, size_t len, uint8_t offset_short,
                     uint8_t offset_long)
{
    if (len <= 55) {
        uint8_t b = (uint8_t)(offset_short + len);
        rlp_put(r, &b, 1);
        return;
    }
    uint8_t be[8];
    int n = 0;
    uint64_t v = len;
    while (v) {
        be[n++] = (uint8_t)(v & 0xff);
        v >>= 8;
    }
    uint8_t head[9];
    head[0] = (uint8_t)(offset_long + n);
    for (int i = 0; i < n; i++)
        head[1 + i] = be[n - 1 - i];
    rlp_put(r, head, (size_t)n + 1);
}

void tb_rlp_bytes(tb_rlp_t *r, const uint8_t *data, size_t len)
{
    if (len == 1 && data[0] < 0x80) { /* канон: байт сам себе */
        rlp_put(r, data, 1);
        return;
    }
    rlp_head(r, len, 0x80, 0xb7);
    rlp_put(r, data, len);
}

void tb_rlp_u64(tb_rlp_t *r, uint64_t v)
{
    if (v == 0) {
        uint8_t b = 0x80;
        rlp_put(r, &b, 1);
        return;
    }
    uint8_t be[8];
    int n = 0;
    while (v) {
        be[n++] = (uint8_t)(v & 0xff);
        v >>= 8;
    }
    /* RLP-целое: big-endian без ведущих нулей как СТРОКА байт */
    uint8_t rev[8];
    for (int i = 0; i < n; i++)
        rev[i] = be[n - 1 - i];
    tb_rlp_bytes(r, rev, (size_t)n);
}

void tb_rlp_list(tb_rlp_t *r, const uint8_t *content, size_t len)
{
    rlp_head(r, len, 0xc0, 0xf7);
    rlp_put(r, content, len);
}

/* ---- EIP-1559 ---- */

/* Полезная нагрузка для подписи/отправки: [chainId, nonce, maxPrio,
 * maxFee, gas, to, value, data, accessList] (+v,r,s для отправки). */
static bool tx_payload(uint8_t *buf, size_t cap, size_t *out_len,
                       const tb_tx1559_t *tx, const uint8_t *sig /* 65:
                                                                    r(32)
                                                                    s(32)
                                                                    parity */)
{
    tb_rlp_t body;
    uint8_t body_buf[512 + 65536];
    if (tx->data_len > sizeof(body_buf) - 512)
        return false;
    tb_rlp_init(&body, body_buf, sizeof(body_buf));

    tb_rlp_u64(&body, tx->chain_id);
    tb_rlp_u64(&body, tx->nonce);
    tb_rlp_u64(&body, tx->max_priority_fee);
    tb_rlp_u64(&body, tx->max_fee);
    tb_rlp_u64(&body, tx->gas_limit);
    tb_rlp_bytes(&body, tx->to.b, 20);
    tb_rlp_u64(&body, tx->value);
    tb_rlp_bytes(&body, tx->data ? tx->data : (const uint8_t *)"",
                 tx->data_len);
    tb_rlp_list(&body, NULL, 0); /* accessList пуст */
    if (sig) {
        tb_rlp_u64(&body, (uint64_t)sig[64]); /* y-parity: 0/1 */
        tb_rlp_bytes(&body, sig, 32);         /* r */
        tb_rlp_bytes(&body, sig + 32, 32);    /* s */
    }
    if (body.overflow)
        return false;

    /* обёртка-список + байт типа EIP-1559 (0x02) */
    tb_rlp_t top;
    tb_rlp_init(&top, buf, cap);
    tb_rlp_list(&top, body_buf, body.len);
    if (top.overflow)
        return false;
    memmove(buf + 1, buf, top.len);
    buf[0] = 0x02;
    *out_len = top.len + 1;
    return true;
}

int tb_tx1559_sign(const tb_key_t *k, const tb_tx1559_t *tx,
                   char *raw_hex, size_t cap, uint8_t tx_hash[32],
                   uint8_t sig65[65], tb_error_t *err)
{
    if (!k || !k->ok || !tx || !raw_hex || cap < 4)
        TB_ERROR_FAIL(err, TB_DOMAIN_CORE, TB_CORE_INVALID_ARG,
                      "sign: аргументы");

    uint8_t buf[512 + 65536];
    size_t len = 0;
    if (!tx_payload(buf, sizeof(buf), &len, tx, NULL))
        TB_ERROR_FAIL(err, TB_DOMAIN_CORE, TB_CORE_OVERFLOW,
                      "sign: rlp не влез");

    uint8_t hash[32];
    tb_keccak256(buf, len, hash);

    secp256k1_ecdsa_recoverable_signature rsig;
    if (!secp256k1_ecdsa_sign_recoverable(sign_ctx(), &rsig, hash,
                                           k->secret, NULL, NULL))
        TB_ERROR_FAIL(err, TB_DOMAIN_CORE, TB_CORE_UNSUPPORTED,
                      "sign: secp256k1 отказ");

    uint8_t sig[65];
    int recid = 0;
    secp256k1_ecdsa_recoverable_signature_serialize_compact(
        sign_ctx(), sig, &recid, &rsig);
    if (recid > 1)
        TB_ERROR_FAIL(err, TB_DOMAIN_CORE, TB_CORE_UNSUPPORTED,
                      "sign: recovery id вне диапазона (не бывает для "
                      "нашего ключа)");
    sig[64] = (uint8_t)recid; /* y-parity */
    if (sig65)
        memcpy(sig65, sig, 65);

    /* low-S: secp256k1 канонизирует по умолчанию (bitcoin-стиль) */

    if (!tx_payload(buf, sizeof(buf), &len, tx, sig))
        TB_ERROR_FAIL(err, TB_DOMAIN_CORE, TB_CORE_OVERFLOW,
                      "sign: rlp+sig не влез");

    tb_keccak256(buf, len, tx_hash);

    size_t n = 0;
    raw_hex[n++] = '0';
    raw_hex[n++] = 'x';
    for (size_t i = 0; i < len && n + 3 < cap; i++)
        n += (size_t)snprintf(raw_hex + n, 3, "%02x", buf[i]);
    raw_hex[n] = '\0';
    return 0;
}

int tb_ecdsa_sign(const tb_key_t *k, const uint8_t hash[32], uint8_t r[32],
                  uint8_t s[32], int *y_parity, tb_error_t *err)
{
    if (!k || !k->ok || !hash || !r || !s || !y_parity)
        TB_ERROR_FAIL(err, TB_DOMAIN_CORE, TB_CORE_INVALID_ARG,
                      "ecdsa: аргументы");

    secp256k1_ecdsa_recoverable_signature rsig;
    if (!secp256k1_ecdsa_sign_recoverable(sign_ctx(), &rsig, hash,
                                           k->secret, NULL, NULL))
        TB_ERROR_FAIL(err, TB_DOMAIN_CORE, TB_CORE_UNSUPPORTED,
                      "ecdsa: secp256k1 отказ");

    uint8_t sig[65];
    int recid = 0;
    secp256k1_ecdsa_recoverable_signature_serialize_compact(
        sign_ctx(), sig, &recid, &rsig);
    memcpy(r, sig, 32);
    memcpy(s, sig + 32, 32);
    *y_parity = recid;
    return 0;
}

int tb_sig_recover_sender(const uint8_t msg_hash[32], const uint8_t r[32],
                          const uint8_t s[32], int y_parity,
                          tb_addr_t *out, tb_error_t *err)
{
    uint8_t sig[65];
    memcpy(sig, r, 32);
    memcpy(sig + 32, s, 32);
    sig[64] = (uint8_t)y_parity;

    secp256k1_ecdsa_recoverable_signature rsig;
    if (!secp256k1_ecdsa_recoverable_signature_parse_compact(
            sign_ctx(), &rsig, sig, y_parity))
        TB_ERROR_FAIL(err, TB_DOMAIN_CORE, TB_CORE_INVALID_ARG,
                      "recover: разбор подписи");


    secp256k1_pubkey pub;
    if (!secp256k1_ecdsa_recover(sign_ctx(), &pub, &rsig, msg_hash))
        TB_ERROR_FAIL(err, TB_DOMAIN_CORE, TB_CORE_UNSUPPORTED,
                      "recover: подпись не восстановилась");

    size_t outlen = 65;
    uint8_t uncomp[65];
    secp256k1_ec_pubkey_serialize(sign_ctx(), uncomp, &outlen, &pub,
                                   SECP256K1_EC_UNCOMPRESSED);

    uint8_t h[32];
    tb_keccak256(uncomp + 1, 64, h);
    memcpy(out->b, h + 12, 20);
    return 0;
}
