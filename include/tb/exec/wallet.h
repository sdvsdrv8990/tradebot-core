/* ==========================================================================
 *  tb/exec/wallet.h — виртуальные балансы и ТОРГОВЫЙ СЛОТ.
 *
 *  Перенос семантики контракта-референса на стек ядра:
 *
 *    контракт                       здесь
 *    -----------------------------  ------------------------------------
 *    «одна транзакция» (all-or-     tb_slot_t: журнал отложенных мутаций;
 *      nothing, revert при любом      commit применяется ЦЕЛИКОМ, rollback
 *      revert)                        отбрасывает всё
 *    transferFrom(msg.sender, ...)  tb_slot_debit (проверяет доступный баланс)
 *    weth.deposit{value}            tb_slot_wrap (нативный -> wrapped)
 *    weth.withdraw + call{value}    tb_slot_unwrap (wrapped -> нативный)
 *    forceApprove(router, amount)   tb_slot_approve (журнал allowance);
 *      ... forceApprove(router, 0)    сброс в ноль — тем же вызовом
 *    receive() { if (msg.sender !=  инвариант: tb_slot_credit НАТИВНОГО
 *      weth) revert NotWeth }          запрещён (TB_EXEC_NOT_WRAPPED) —
 *                                      нативный приходит только из unwrap
 *
 *  Кошелёк — это виртуальные балансы трейдера (основа модуля 5B:
 *  Trade Slot Engine и теневые сделки). Non-custodial в том же смысле,
 *  что и контракт: механизм не имеет привилегий двигать балансы мимо
 *  слота — все мутации видны в журнале слота и применяются атомарно.
 *
 *  Контракт потоков: однопоточный (поток-владелец движка), как весь реестр.
 * ========================================================================== */
#ifndef TB_EXEC_WALLET_H
#define TB_EXEC_WALLET_H

#include <stdbool.h>
#include <stddef.h>
#include "tb/core/error.h"
#include "tb/exec/codes.h"
#include "tb/exec/route.h"
#include "tb/exec/u256.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TB_WALLET_MAX_ASSETS 32
#define TB_SLOT_MAX_ENTRIES  16
#define TB_SLOT_MAX_ALLOW    4

typedef struct tb_wallet tb_wallet_t;
typedef struct tb_slot   tb_slot_t;

tb_wallet_t *tb_wallet_create(tb_error_t *err);
void         tb_wallet_destroy(tb_wallet_t *w);

/* Зачислить актив на баланс (депозит трейдера; нативный — тоже можно:
 * это внешний вход, а не расчёт по сделке). 0 — успех. */
int tb_wallet_deposit(tb_wallet_t *w, const tb_addr_t *asset,
                      const tb_u256_t *amount, tb_error_t *err);

/* Текущий (зафиксированный) баланс актива. */
tb_u256_t tb_wallet_balance(const tb_wallet_t *w, const tb_addr_t *asset);

/* Инвентаризация: число активов; актив по индексу с балансом. */
size_t tb_wallet_asset_count(const tb_wallet_t *w);
bool tb_wallet_asset_at(const tb_wallet_t *w, size_t i, tb_addr_t *asset,
                        tb_u256_t *balance);

/* Зафиксированный allowance маршрутизатору (0, если не назначался). */
tb_u256_t tb_wallet_allowance(const tb_wallet_t *w, const tb_addr_t *asset);

/* ------------------------------------------------------------------ */
/*  Торговый слот                                                       */
/* ------------------------------------------------------------------ */

/* Открыть слот (журнал мутаций поверх кошелька). */
tb_slot_t *tb_slot_open(tb_wallet_t *w, tb_error_t *err);

/* Списать актив: доступно = баланс + незакоммиченные зачисления слота -
 * незакоммиченные списания. Не хватает — TB_EXEC_INSUFFICIENT_BALANCE,
 * слот не меняется. */
int tb_slot_debit(tb_slot_t *s, const tb_addr_t *asset,
                  const tb_u256_t *amount, tb_error_t *err);

/* Зачислить актив. НАТИВНЫЙ АКТИВ ЗАПРЕЩЁН (инвариант receive()):
 * нативный появляется только через tb_slot_unwrap. */
int tb_slot_credit(tb_slot_t *s, const tb_addr_t *asset,
                   const tb_u256_t *amount, tb_error_t *err);

/* Allowance маршрутизатору внутри слота (forceApprove): значение живёт
 * в журнале, фиксируется коммитом. amount = 0 — сброс. */
int tb_slot_approve(tb_slot_t *s, const tb_addr_t *asset,
                    const tb_u256_t *amount, tb_error_t *err);
tb_u256_t tb_slot_allowance(const tb_slot_t *s, const tb_addr_t *asset);

/* Завернуть нативный в wrapped (deposit): дебет нативного + кредит wrapped. */
int tb_slot_wrap(tb_slot_t *s, const tb_addr_t *wrapped,
                 const tb_u256_t *amount, tb_error_t *err);

/* Развернуть wrapped в нативный (withdraw + перевод): дебет wrapped +
 * кредит нативного. Именно отсюда нативный попадает в слот. */
int tb_slot_unwrap(tb_slot_t *s, const tb_addr_t *wrapped,
                   const tb_u256_t *amount, tb_error_t *err);

/* Доступный баланс с учётом журнала слота (для инспекции/следов). */
tb_u256_t tb_slot_available(const tb_slot_t *s, const tb_addr_t *asset);

/* Зафиксировать: применить ВСЕ мутации журнала к кошельку атомарно.
 * Слот закрывается и освобождается. 0 — успех. */
int tb_slot_commit(tb_slot_t *s, tb_error_t *err);

/* Отбросить журнал целиком (аналог revert) и освободить слот. */
void tb_slot_rollback(tb_slot_t *s);

bool tb_slot_is_open(const tb_slot_t *s);

#ifdef __cplusplus
}
#endif
#endif /* TB_EXEC_WALLET_H */
