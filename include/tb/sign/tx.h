/* ==========================================================================
 *  tb/sign/tx.h — ключ бота и подпись EIP-1559 (v0.4).
 *
 *  Чистые функции без сети: сборка+подпись транзакции детерминированы,
 *  тестируются офлайн (деривация адреса, RLP-векторы, самопроверка
 *  восстановления отправителя). Отправка (nonce/газ/sendRaw) — слой
 *  исполнителя (safe_exec), здесь только криптография и кодирование.
 * ==========================================================================
 */
#ifndef TB_SIGN_TX_H
#define TB_SIGN_TX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "tb/core/error.h"
#include "tb/exec/route.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Ключ (горячий ключ бота; конверт риска = содержимое Safe) ---- */
typedef struct tb_key {
    uint8_t   secret[32];
    uint8_t   pub[64];    /* несжатый, без префикса 0x04 */
    tb_addr_t address;    /* keccak(pub)[12:] */
    bool      ok;
} tb_key_t;

/* Из hex (0x…/без, 64 символа). */
int tb_key_from_hex(tb_key_t *k, const char *hex32, tb_error_t *err);

/* Из переменной окружения. */
int tb_key_from_env(tb_key_t *k, const char *var, tb_error_t *err);

/* ---- Минимальный RLP-кодер ---- */
typedef struct tb_rlp {
    uint8_t *buf;
    size_t   cap;
    size_t   len;
    bool     overflow;
} tb_rlp_t;

void tb_rlp_init(tb_rlp_t *r, uint8_t *buf, size_t cap);
void tb_rlp_bytes(tb_rlp_t *r, const uint8_t *data, size_t len);
void tb_rlp_u64(tb_rlp_t *r, uint64_t v);
/* Список: собрать содержимое во временный rlp и вложить. */
void tb_rlp_list(tb_rlp_t *r, const uint8_t *content, size_t len);

/* ---- EIP-1559 (dynamic fee) ---- */
typedef struct tb_tx1559 {
    uint64_t   chain_id;
    uint64_t   nonce;
    uint64_t   max_priority_fee; /* wei */
    uint64_t   max_fee;          /* wei */
    uint64_t   gas_limit;
    tb_addr_t  to;
    uint64_t   value;            /* wei */
    const uint8_t *data;         /* calldata (может быть NULL) */
    size_t     data_len;
} tb_tx1559_t;

/* Собрать, подписать (low-S, y-parity) и вернуть raw-транзакцию hex
 * ("0x…") + хеш транзакции. sig65 (опционально): r(32) s(32) parity(1).
 * 0 — успех. */
int tb_tx1559_sign(const tb_key_t *k, const tb_tx1559_t *tx,
                   char *raw_hex, size_t cap, uint8_t tx_hash[32],
                   uint8_t sig65[65], tb_error_t *err);

/* Голый ECDSA над хешем (EIP-712/личные сообщения): r, s, y-parity. */
int tb_ecdsa_sign(const tb_key_t *k, const uint8_t hash[32], uint8_t r[32],
                  uint8_t s[32], int *y_parity, tb_error_t *err);

/* Восстановить адрес отправителя из подписи (самопроверка/тесты). */
int tb_sig_recover_sender(const uint8_t msg_hash[32], const uint8_t r[32],
                          const uint8_t s[32], int y_parity,
                          tb_addr_t *out, tb_error_t *err);

#ifdef __cplusplus
}
#endif
#endif /* TB_SIGN_TX_H */
