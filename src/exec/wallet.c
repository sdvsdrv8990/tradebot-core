#define TB_CURRENT_GROUP TB_G_EXEC /* зона ответственности файла */
/* ==========================================================================
 *  tb/exec/wallet.c — виртуальные балансы и торговый слот.
 * ========================================================================== */
#include "tb/exec/wallet.h"

#include <stdlib.h>
#include <string.h>

/* ---- Кошелёк: строка актива ---- */
typedef struct {
    tb_addr_t asset;
    tb_u256_t balance;
    tb_u256_t allowance; /* маршрутизатору (единственный spender в v0.2) */
} tb_wrow_t;

struct tb_wallet {
    tb_wrow_t rows[TB_WALLET_MAX_ASSETS];
    size_t    n;
};

/* ---- Слот: журнал отложенных мутаций ---- */
typedef struct {
    tb_addr_t asset;
    tb_u256_t debited;  /* суммарные списания */
    tb_u256_t credited; /* суммарные зачисления */
} tb_srow_t;

typedef struct {
    tb_addr_t asset;
    tb_u256_t allowance; /* финальное значение на момент коммита */
} tb_arow_t;

struct tb_slot {
    tb_wallet_t *wallet;   /* владелец балансов */
    tb_srow_t    rows[TB_SLOT_MAX_ENTRIES];
    size_t       nrows;
    tb_arow_t    allows[TB_SLOT_MAX_ALLOW];
    size_t       nallows;
    bool         open;
};

/* ======================================================================
 *  Кошелёк
 * ====================================================================== */
static tb_wrow_t *wrow_find(tb_wallet_t *w, const tb_addr_t *asset)
{
    for (size_t i = 0; i < w->n; i++)
        if (tb_addr_eq(&w->rows[i].asset, asset))
            return &w->rows[i];
    return NULL;
}

tb_wallet_t *tb_wallet_create(tb_error_t *err)
{
    tb_wallet_t *w = calloc(1, sizeof(*w));
    if (!w)
        TB_ERROR_SET(err, TB_DOMAIN_CORE, TB_CORE_NO_MEMORY,
                     "кошелёк: нет памяти");
    return w;
}

void tb_wallet_destroy(tb_wallet_t *w)
{
    free(w);
}

int tb_wallet_deposit(tb_wallet_t *w, const tb_addr_t *asset,
                      const tb_u256_t *amount, tb_error_t *err)
{
    if (!w || !asset || !amount) {
        TB_ERROR_FAIL(err, TB_DOMAIN_CORE, TB_CORE_INVALID_ARG,
                      "депозит: пустой аргумент");
    }
    if (tb_u256_is_zero(amount))
        return 0; /* депозит нуля — допустим и ничего не меняет */

    tb_wrow_t *row = wrow_find(w, asset);
    if (!row) {
        if (w->n >= TB_WALLET_MAX_ASSETS)
            TB_ERROR_FAIL(err, TB_DOMAIN_USER, TB_EXEC_CAPACITY,
                          "кошелёк: таблица активов полна (%d)",
                          TB_WALLET_MAX_ASSETS);
        row = &w->rows[w->n++];
        row->asset    = *asset;
        row->balance  = tb_u256_from_u64(0);
        row->allowance = tb_u256_from_u64(0);
    }
    if (!tb_u256_add(&row->balance, &row->balance, amount))
        TB_ERROR_FAIL(err, TB_DOMAIN_USER, TB_EXEC_OVERFLOW,
                      "кошелёк: переполнение баланса при депозите");
    return 0;
}

tb_u256_t tb_wallet_balance(const tb_wallet_t *w, const tb_addr_t *asset)
{
    tb_u256_t zero = tb_u256_from_u64(0);
    if (!w || !asset)
        return zero;
    tb_wrow_t *row = wrow_find((tb_wallet_t *)w, asset);
    return row ? row->balance : zero;
}

size_t tb_wallet_asset_count(const tb_wallet_t *w)
{
    return w ? w->n : 0;
}

bool tb_wallet_asset_at(const tb_wallet_t *w, size_t i, tb_addr_t *asset,
                        tb_u256_t *balance)
{
    if (!w || i >= w->n)
        return false;
    if (asset)
        *asset = w->rows[i].asset;
    if (balance)
        *balance = w->rows[i].balance;
    return true;
}

tb_u256_t tb_wallet_allowance(const tb_wallet_t *w, const tb_addr_t *asset)
{
    tb_u256_t zero = tb_u256_from_u64(0);
    if (!w || !asset)
        return zero;
    tb_wrow_t *row = wrow_find((tb_wallet_t *)w, asset);
    return row ? row->allowance : zero;
}

/* ======================================================================
 *  Слот
 * ====================================================================== */
static tb_srow_t *srow_find(tb_slot_t *s, const tb_addr_t *asset)
{
    for (size_t i = 0; i < s->nrows; i++)
        if (tb_addr_eq(&s->rows[i].asset, asset))
            return &s->rows[i];
    return NULL;
}

static tb_srow_t *srow_get(tb_slot_t *s, const tb_addr_t *asset, tb_error_t *err)
{
    tb_srow_t *row = srow_find(s, asset);
    if (row)
        return row;
    if (s->nrows >= TB_SLOT_MAX_ENTRIES) {
        TB_ERROR_SET(err, TB_DOMAIN_USER, TB_EXEC_CAPACITY,
                     "слот: журнал мутаций полон (%d записей)",
                     TB_SLOT_MAX_ENTRIES);
        return NULL;
    }
    row        = &s->rows[s->nrows++];
    row->asset = *asset;
    row->debited  = tb_u256_from_u64(0);
    row->credited = tb_u256_from_u64(0);
    return row;
}

tb_slot_t *tb_slot_open(tb_wallet_t *w, tb_error_t *err)
{
    if (!w) {
        TB_ERROR_SET(err, TB_DOMAIN_CORE, TB_CORE_INVALID_ARG,
                     "слот: кошелёк обязателен");
        return NULL;
    }
    tb_slot_t *s = calloc(1, sizeof(*s));
    if (!s) {
        TB_ERROR_SET(err, TB_DOMAIN_CORE, TB_CORE_NO_MEMORY,
                     "слот: нет памяти");
        return NULL;
    }
    s->wallet = w;
    s->open   = true;
    return s;
}

bool tb_slot_is_open(const tb_slot_t *s)
{
    return s && s->open;
}

tb_u256_t tb_slot_available(const tb_slot_t *s, const tb_addr_t *asset)
{
    tb_u256_t avail = tb_wallet_balance(s ? s->wallet : NULL, asset);
    if (!s)
        return avail;
    const tb_srow_t *row = srow_find((tb_slot_t *)s, asset);
    if (row) {
        tb_u256_t t;
        if (tb_u256_add(&t, &avail, &row->credited))
            avail = t;
        if (tb_u256_sub(&t, &avail, &row->debited))
            avail = t;
    }
    return avail;
}

int tb_slot_debit(tb_slot_t *s, const tb_addr_t *asset,
                  const tb_u256_t *amount, tb_error_t *err)
{
    if (!s || !s->open || !asset || !amount)
        TB_ERROR_FAIL(err, TB_DOMAIN_CORE, TB_CORE_INVALID_ARG,
                      "слот: дебет с пустым аргументом");
    if (tb_u256_is_zero(amount))
        return 0;

    tb_u256_t avail = tb_slot_available(s, asset);
    if (tb_u256_cmp(&avail, amount) < 0)
        TB_ERROR_FAIL(err, TB_DOMAIN_USER, TB_EXEC_INSUFFICIENT_BALANCE,
                      "слот: не хватает актива (доступно меньше списания)");

    tb_srow_t *row = srow_get(s, asset, err);
    if (!row)
        return -1;
    if (!tb_u256_add(&row->debited, &row->debited, amount))
        TB_ERROR_FAIL(err, TB_DOMAIN_USER, TB_EXEC_OVERFLOW,
                      "слот: переполнение журнала списаний");
    return 0;
}

/* Внутреннее зачисление: cred нативного разрешено только из unwrap. */
static int slot_credit_internal(tb_slot_t *s, const tb_addr_t *asset,
                                const tb_u256_t *amount, tb_error_t *err)
{
    if (tb_u256_is_zero(amount))
        return 0;
    tb_srow_t *row = srow_get(s, asset, err);
    if (!row)
        return -1;
    if (!tb_u256_add(&row->credited, &row->credited, amount))
        TB_ERROR_FAIL(err, TB_DOMAIN_USER, TB_EXEC_OVERFLOW,
                      "слот: переполнение журнала зачислений");
    return 0;
}

int tb_slot_credit(tb_slot_t *s, const tb_addr_t *asset,
                   const tb_u256_t *amount, tb_error_t *err)
{
    if (!s || !s->open || !asset || !amount)
        TB_ERROR_FAIL(err, TB_DOMAIN_CORE, TB_CORE_INVALID_ARG,
                      "слот: кредит с пустым аргументом");
    /* Инвариант receive() из контракта: нативный актив извне не зачисляется —
     * только разворачиванием wrapped (tb_slot_unwrap). */
    if (tb_addr_is_native(asset))
        TB_ERROR_FAIL(err, TB_DOMAIN_USER, TB_EXEC_NOT_WRAPPED,
                      "слот: нативный актив зачисляется только через unwrap "
                      "(инвариант receive())");
    return slot_credit_internal(s, asset, amount, err);
}

int tb_slot_approve(tb_slot_t *s, const tb_addr_t *asset,
                    const tb_u256_t *amount, tb_error_t *err)
{
    if (!s || !s->open || !asset || !amount)
        TB_ERROR_FAIL(err, TB_DOMAIN_CORE, TB_CORE_INVALID_ARG,
                      "слот: approve с пустым аргументом");

    tb_arow_t *row = NULL;
    for (size_t i = 0; i < s->nallows; i++)
        if (tb_addr_eq(&s->allows[i].asset, asset))
            row = &s->allows[i];
    if (!row) {
        if (s->nallows >= TB_SLOT_MAX_ALLOW)
            TB_ERROR_FAIL(err, TB_DOMAIN_USER, TB_EXEC_CAPACITY,
                          "слот: таблица allowance полна (%d)",
                          TB_SLOT_MAX_ALLOW);
        /* Не добавляем строку для сброса в ноль актива без allowance. */
        if (tb_u256_is_zero(amount))
            return 0;
        row = &s->allows[s->nallows++];
        row->asset = *asset;
    }
    row->allowance = *amount;
    return 0;
}

tb_u256_t tb_slot_allowance(const tb_slot_t *s, const tb_addr_t *asset)
{
    tb_u256_t zero = tb_u256_from_u64(0);
    if (!s)
        return zero;
    for (size_t i = 0; i < s->nallows; i++)
        if (tb_addr_eq(&s->allows[i].asset, asset))
            return s->allows[i].allowance;
    return zero;
}

int tb_slot_wrap(tb_slot_t *s, const tb_addr_t *wrapped,
                 const tb_u256_t *amount, tb_error_t *err)
{
    if (!s || !s->open || !wrapped || !amount)
        TB_ERROR_FAIL(err, TB_DOMAIN_CORE, TB_CORE_INVALID_ARG,
                      "wrap: пустой аргумент");
    if (tb_addr_is_native(wrapped))
        TB_ERROR_FAIL(err, TB_DOMAIN_USER, TB_EXEC_NOT_WRAPPED,
                      "wrap: wrapped-токен не может быть нулевым адресом");
    if (tb_u256_is_zero(amount))
        return 0;

    tb_addr_t native = tb_addr_zero();
    if (tb_slot_debit(s, &native, amount, err) != 0)
        return -1;
    return slot_credit_internal(s, wrapped, amount, err);
}

int tb_slot_unwrap(tb_slot_t *s, const tb_addr_t *wrapped,
                   const tb_u256_t *amount, tb_error_t *err)
{
    if (!s || !s->open || !wrapped || !amount)
        TB_ERROR_FAIL(err, TB_DOMAIN_CORE, TB_CORE_INVALID_ARG,
                      "unwrap: пустой аргумент");
    if (tb_addr_is_native(wrapped))
        TB_ERROR_FAIL(err, TB_DOMAIN_USER, TB_EXEC_NOT_WRAPPED,
                      "unwrap: wrapped-токен не может быть нулевым адресом");
    if (tb_u256_is_zero(amount))
        return 0;

    if (tb_slot_debit(s, wrapped, amount, err) != 0)
        return -1;
    tb_addr_t native = tb_addr_zero();
    return slot_credit_internal(s, &native, amount, err);
}

int tb_slot_commit(tb_slot_t *s, tb_error_t *err)
{
    if (!s || !s->open)
        TB_ERROR_FAIL(err, TB_DOMAIN_CORE, TB_CORE_INVALID_ARG,
                      "коммит: слот не открыт");

    /* Сначала — досоздать строки кошелька под все затронутые активы,
     * чтобы коммит не упал на полпути (ёмкость проверяем до мутаций). */
    for (size_t i = 0; i < s->nrows; i++) {
        if (!wrow_find(s->wallet, &s->rows[i].asset) &&
            s->wallet->n >= TB_WALLET_MAX_ASSETS)
            TB_ERROR_FAIL(err, TB_DOMAIN_USER, TB_EXEC_CAPACITY,
                          "коммит: таблица активов кошелька полна");
    }
    for (size_t i = 0; i < s->nallows; i++) {
        if (!wrow_find(s->wallet, &s->allows[i].asset) &&
            s->wallet->n >= TB_WALLET_MAX_ASSETS)
            TB_ERROR_FAIL(err, TB_DOMAIN_USER, TB_EXEC_CAPACITY,
                          "коммит: таблица активов кошелька полна");
    }

    for (size_t i = 0; i < s->nrows; i++) {
        tb_srow_t *sr = &s->rows[i];
        if (tb_u256_is_zero(&sr->debited) && tb_u256_is_zero(&sr->credited))
            continue;
        tb_wrow_t *wr = wrow_find(s->wallet, &sr->asset);
        if (!wr) {
            wr = &s->wallet->rows[s->wallet->n++];
            wr->asset     = sr->asset;
            wr->balance   = tb_u256_from_u64(0);
            wr->allowance = tb_u256_from_u64(0);
        }
        tb_u256_t t = wr->balance;
        if (!tb_u256_is_zero(&sr->credited) &&
            !tb_u256_add(&t, &t, &sr->credited))
            TB_ERROR_FAIL(err, TB_DOMAIN_USER, TB_EXEC_OVERFLOW,
                          "коммит: переполнение баланса при применении кредита");
        if (!tb_u256_is_zero(&sr->debited) &&
            !tb_u256_sub(&t, &t, &sr->debited))
            TB_ERROR_FAIL(err, TB_DOMAIN_USER, TB_EXEC_VALUE_MISMATCH,
                          "коммит: сведение дебета/кредита не сошлось "
                          "(инвариант слота)");
        wr->balance = t;
    }
    for (size_t i = 0; i < s->nallows; i++) {
        tb_wrow_t *wr = wrow_find(s->wallet, &s->allows[i].asset);
        if (!wr) {
            wr = &s->wallet->rows[s->wallet->n++];
            wr->asset     = s->allows[i].asset;
            wr->balance   = tb_u256_from_u64(0);
            wr->allowance = tb_u256_from_u64(0);
        }
        wr->allowance = s->allows[i].allowance;
    }

    s->open = false;
    free(s);
    return 0;
}

void tb_slot_rollback(tb_slot_t *s)
{
    if (!s)
        return;
    s->open = false;
    free(s);
}
