/* ==========================================================================
 *  tb/exec/safe_exec.h — живое исполнение из Safe (v0.4).
 *
 *  Safe 1-из-2: бот-владелец исполняет свопы через execTransaction —
 *  средства не покидают контракт, прибыль остаётся в нём. Риск-конверт
 *  ключа бота = содержимое Safe (горячий ключ).
 *
 *  Здесь: обёртка SafeTx (EIP-712) поверх подписи EIP-1559 (tb_sign),
 *  сборка calldata SwapRouter02 exactInput и Safe execTransaction,
 *  RPC-помощники (nonce/газ/отправка/receipt/balanceOf/allowance).
 *  Используется DEX-адаптером в live-режиме; конвейер не меняется.
 * ==========================================================================
 */
#ifndef TB_EXEC_SAFE_EXEC_H
#define TB_EXEC_SAFE_EXEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "tb/core/error.h"
#include "tb/exec/route.h"
#include "tb/exec/u256.h"
#include "tb/net/http.h"
#include "tb/sign/tx.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tb_safe_cfg {
    char       rpc_url[TB_HTTP_URL_MAX];
    char       rpc_host_pinned[TB_HTTP_HOST_MAX];
    tb_addr_t  safe;      /* адрес Safe (verifyingContract) */
    tb_addr_t  router;    /* SwapRouter02 */
    tb_key_t   key;       /* ключ бота-владельца (ok=true) */
    uint64_t   chain_id;  /* 0 -> eth_chainId */
    uint64_t   gas_limit; /* 0 -> eth_estimateGas + 30% запас */
    uint64_t   max_fee_wei; /* потолок base+priority; 0 -> без потолка */
    uint32_t   timeout_ms;  /* 0 -> 10000 */
} tb_safe_cfg_t;

/* ---- Calldata ---- */

/* SwapRouter02.exactInput((bytes path, address recipient, uint amountIn,
 * uint amountOutMinimum)). Возврат — длина. */
size_t tb_safe_build_exact_input(uint8_t *buf, size_t cap,
                                 const tb_route_t *route,
                                 const tb_addr_t *safe,
                                 const tb_u256_t *amount_in,
                                 const tb_u256_t *min_out);

/* Safe.execTransaction(to, value, data, op=0, 0,0,0, 0,0, sig). */
size_t tb_safe_build_exec_tx(uint8_t *buf, size_t cap, const tb_addr_t *to,
                             uint64_t value, const uint8_t *data,
                             size_t data_len, const uint8_t sig65[65]);

/* ---- EIP-712 SafeTx ---- */
int tb_safe_tx_hash(const tb_safe_cfg_t *cfg, const tb_addr_t *to,
                    uint64_t value, const uint8_t *data, size_t data_len,
                    uint64_t safe_nonce, uint8_t hash[32], tb_error_t *err);

/* Подпись SafeTx ключом бота (v = parity + 27, ECDSA-владелец). */
int tb_safe_sign(const tb_safe_cfg_t *cfg, const uint8_t safe_tx_hash[32],
                 uint8_t sig65[65], tb_error_t *err);

/* ---- RPC (блокирующие; вызывать из воркеров/submit) ---- */

int tb_safe_chain_id(tb_http_t *h, const tb_safe_cfg_t *c, uint64_t *out,
                     tb_error_t *err);
/* Нонс Safe (eth_call nonce() контракта Safe — не аккаунт-нонс!). */
int tb_safe_nonce(tb_http_t *h, const tb_safe_cfg_t *c, uint64_t *out,
                  tb_error_t *err);
int tb_safe_balance_of(tb_http_t *h, const tb_safe_cfg_t *c,
                       const tb_addr_t *token, const tb_addr_t *holder,
                       tb_u256_t *out, tb_error_t *err);
int tb_safe_allowance(tb_http_t *h, const tb_safe_cfg_t *c,
                      const tb_addr_t *token, const tb_addr_t *owner,
                      const tb_addr_t *spender, tb_u256_t *out,
                      tb_error_t *err);

/* Собрать, подписать и отправить execTransaction. Возврат — хеш
 * транзакции. Синхронно делает 3-5 RPC-вызовов. */
int tb_safe_send_exec(tb_http_t *h, const tb_safe_cfg_t *c, const tb_addr_t *to,
                      uint64_t value, const uint8_t *data, size_t data_len,
                      uint8_t tx_hash[32], tb_error_t *err);

/* Receipt: is_null — ещё не смайнился; иначе status 0/1. */
int tb_safe_receipt(tb_http_t *h, const tb_safe_cfg_t *c,
                    const uint8_t tx_hash[32], bool *is_null, int *status,
                    tb_error_t *err);

#ifdef __cplusplus
}
#endif
#endif /* TB_EXEC_SAFE_EXEC_H */
