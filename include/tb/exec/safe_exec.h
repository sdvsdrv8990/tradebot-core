/* ==========================================================================
 *  tb/exec/safe_exec.h — подготовка исполнения из Safe (v0.6, p0.1).
 *
 *  Модель БЕЗКЛЮЧЕВАЯ (propose-only): приватного ключа бота нет нигде,
 *  Safe 1-из-1 — единственный владелец человек. Бот читает цепь, готовит
 *  SafeTx (calldata + EIP-712-хеш для сверки) и публикует предложение:
 *  артефакт (файл/журнал/консоль) + очередь Safe UI через Transaction
 *  Service. Подписывает и исполняет человек своим кошельком; бот узнаёт
 *  об исполнении по росту nonce Safe, результат считает дельтой баланса.
 *
 *  Здесь: сборка calldata (SwapRouter02 exactInput, ERC20 approve),
 *  EIP-712 SafeTx-хеш (эталон сверки для кошелька), RPC-помощники
 *  (chainId/nonce/balanceOf/allowance), публикация в Transaction
 *  Service. Отправка транзакций и приватные ключи отсутствуют как класс.
 * ========================================================================== */
#ifndef TB_EXEC_SAFE_EXEC_H
#define TB_EXEC_SAFE_EXEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "tb/core/error.h"
#include "tb/exec/route.h"
#include "tb/exec/u256.h"
#include "tb/net/http.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Окно наблюдения предложения: сколько бот ждёт подписей человека,
 * прежде чем снять локальное наблюдение (на цепи ничего не происходит;
 * человек может исполнить и позже — min_out в calldata защищает его). */
#define TB_SAFE_PROPOSAL_WATCH_MS 1800000ull /* 30 минут */

/* Рабочий лимит approve WETH->роутер: max_trade_wei x MULT — ограничен,
 * но не требует approve на каждую сделку (обычная практика роутеров:
 * либо лимит, либо бесконечность; мы — за ограниченный). */
#define TB_SAFE_APPROVE_MULT 100ull

typedef struct tb_safe_cfg {
    char       rpc_url[TB_HTTP_URL_MAX];
    char       rpc_host_pinned[TB_HTTP_HOST_MAX];
    tb_addr_t  safe;      /* адрес Safe (verifyingContract) */
    tb_addr_t  router;    /* SwapRouter02 */
    uint64_t   chain_id;  /* 0 -> eth_chainId */
    uint64_t   gas_limit; /* зарезервировано (propose не оценивает газ) */
    uint64_t   max_fee_wei; /* зарезервировано (propose не отправляет) */
    uint32_t   timeout_ms;  /* 0 -> 10000 */
    /* Transaction Service: публикация предложения в очередь Safe UI.
     * Пусто — выключено (только артефакт-файл). */
    char       tx_service_url[TB_HTTP_URL_MAX];
} tb_safe_cfg_t;

/* Дефолтные эндпоинты очереди по цепи (config подставляет при
 * отсутствии поля; "off" в конфиге = выключить публикации). */
#define TB_SAFE_TXSVC_MAINNET "https://safe-transaction-mainnet.safe.global"
#define TB_SAFE_TXSVC_BASE    "https://safe-transaction-base.safe.global"

/* ---- Calldata ---- */

/* SwapRouter02.exactInput((bytes path, address recipient, uint amountIn,
 * uint amountOutMinimum)). Возврат — длина. */
size_t tb_safe_build_exact_input(uint8_t *buf, size_t cap,
                                 const tb_route_t *route,
                                 const tb_addr_t *safe,
                                 const tb_u256_t *amount_in,
                                 const tb_u256_t *min_out);

/* ERC20.approve(address spender, uint256 amount). Возврат — длина. */
size_t tb_safe_build_approve(uint8_t *buf, size_t cap,
                             const tb_addr_t *spender,
                             const tb_u256_t *amount);

/* Safe.execTransaction(to, value, data, op=0, 0,0,0, 0,0, sig). */
size_t tb_safe_build_exec_tx(uint8_t *buf, size_t cap, const tb_addr_t *to,
                             uint64_t value, const uint8_t *data,
                             size_t data_len, const uint8_t sig65[65]);

/* ---- EIP-712 SafeTx ---- */
int tb_safe_tx_hash(const tb_safe_cfg_t *cfg, const tb_addr_t *to,
                    uint64_t value, const uint8_t *data, size_t data_len,
                    uint64_t safe_nonce, uint8_t hash[32], tb_error_t *err);

/* ---- Предложение SafeTx (propose-only) ---- */

typedef struct tb_safe_proposal {
    uint64_t  chain_id;
    tb_addr_t safe;        /* verifyingContract */
    tb_addr_t to;          /* цель внутреннего вызова (роутер/токен) */
    uint64_t  value;       /* всегда 0 в наших сценариях */
    uint8_t   data[512];   /* внутренний calldata (exactInput/approve) */
    size_t    data_len;
    uint64_t  safe_nonce;  /* nonce Safe на момент подготовки */
    uint8_t   safe_tx_hash[32]; /* EIP-712 SafeTx-хеш — эталон сверки */
    char      kind[16];    /* "trade" | "approve" (метка вызывающего) */
    uint64_t  created_ms;
    uint64_t  expires_at_ms; /* окно наблюдения (0 -> WATCH_MS) */
} tb_safe_proposal_t;

/* Подготовить предложение: chainId (если 0 — с цепи) + nonce + хеш +
 * подпись. Транзакции в сеть не отправляет. 0 — успех. */
int tb_safe_propose_exec(tb_http_t *h, const tb_safe_cfg_t *c,
                         const tb_addr_t *to, uint64_t value,
                         const uint8_t *data, size_t data_len,
                         uint64_t now_ms, tb_safe_proposal_t *out,
                         tb_error_t *err);

/* Готовый calldata execTransaction с подписью бота (для инструментов
 * человека: кошелёк добавит свою подпись и отправит). Возврат — длина. */
size_t tb_safe_proposal_exec_calldata(const tb_safe_proposal_t *p,
                                      uint8_t *buf, size_t cap);

/* Публикация предложения в очередь Safe UI (Transaction Service):
 * POST /api/v1/safes/{safe}/multisig-transactions/ без подписи — при
 * пороге 1 одно подтверждение человека в UI = исполнение.
 * 0 — принято (в очереди); ошибка — вызывающий деградирует на
 * артефакт-файл, конвейер не останавливать. */
int tb_safe_service_propose(tb_http_t *h, const tb_safe_cfg_t *c,
                            const tb_safe_proposal_t *p, tb_error_t *err);

/* JSON-артефакт предложения (все параметры + хеш + verify-блок).
 * Возврат — записанная длина без NUL. */
size_t tb_safe_proposal_json(const tb_safe_proposal_t *p, char *out,
                             size_t cap);

/* ---- RPC (блокирующие; вызывать из воркеров/submit) ---- */

int tb_safe_chain_id(tb_http_t *h, const tb_safe_cfg_t *c, uint64_t *out,
                     tb_error_t *err);
/* Нонс Safe (eth_call nonce() контракта Safe — не аккаунт-нонс!):
 * рост nonce = транзакция исполнена (или вытеснена) — сигнал завершить
 * наблюдение за предложением. */
int tb_safe_nonce(tb_http_t *h, const tb_safe_cfg_t *c, uint64_t *out,
                  tb_error_t *err);
int tb_safe_balance_of(tb_http_t *h, const tb_safe_cfg_t *c,
                       const tb_addr_t *token, const tb_addr_t *holder,
                       tb_u256_t *out, tb_error_t *err);
int tb_safe_allowance(tb_http_t *h, const tb_safe_cfg_t *c,
                      const tb_addr_t *token, const tb_addr_t *owner,
                      const tb_addr_t *spender, tb_u256_t *out,
                      tb_error_t *err);

#ifdef __cplusplus
}
#endif
#endif /* TB_EXEC_SAFE_EXEC_H */
