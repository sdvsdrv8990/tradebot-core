/* tests/test_wallet.c — виртуальные балансы и торговый слот. */
#include "harness.h"
#include "tb/exec/u256.h"
#include "tb/exec/wallet.h"

#include <string.h>

static tb_u256_t dec(const char *s)
{
    tb_u256_t v;
    CHECK(tb_u256_from_dec(&v, s, strlen(s), NULL) == 0);
    return v;
}

static tb_addr_t A(const char *hex)
{
    tb_addr_t a;
    CHECK(tb_addr_from_hex(&a, hex, NULL) == 0);
    return a;
}

/* Сверка суммы с десятичной строкой. */
static void AMT_IS(const tb_u256_t *got, const char *want)
{
    char buf[80];
    tb_u256_to_dec(got, buf, sizeof(buf));
    if (strcmp(buf, want) != 0) {
        fprintf(stderr, "FAIL %s:%d: суммы %s != %s\n", __FILE__, __LINE__,
                buf, want);
        exit(1);
    }
}

int main(void)
{
    tb_error_t err = TB_ERROR_INIT;

    tb_addr_t native = tb_addr_zero();
    tb_addr_t weth   = A("0xeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee");
    tb_addr_t tok_a  = A("0xaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    tb_addr_t tok_b  = A("0xbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");

    /* ---- депозиты и балансы ---- */
    tb_wallet_t *w = tb_wallet_create(&err);
    CHECK(w != NULL);
    tb_u256_t amt;

    amt = dec("1000000000000000000");
    CHECK(tb_wallet_deposit(w, &tok_a, &amt, &err) == 0);
    amt = dec("500000000000000000");
    CHECK(tb_wallet_deposit(w, &tok_a, &amt, &err) == 0);
    amt = dec("7000000000000000000");
    CHECK(tb_wallet_deposit(w, &native, &amt, &err) == 0);

    {
        tb_u256_t b = tb_wallet_balance(w, &tok_a);
        AMT_IS(&b, "1500000000000000000");
        b = tb_wallet_balance(w, &native);
        AMT_IS(&b, "7000000000000000000");
        b = tb_wallet_balance(w, &tok_b);
        CHECK(tb_u256_is_zero(&b));
    }
    CHECK(tb_wallet_asset_count(w) == 2);

    /* ---- слот: стэйтинг до коммита ---- */
    tb_slot_t *s = tb_slot_open(w, &err);
    CHECK(s != NULL);

    amt = dec("1000000000000000000");
    CHECK(tb_slot_debit(s, &tok_a, &amt, &err) == 0);
    { /* баланс НЕ изменился — мутация ещё в журнале */
        tb_u256_t b = tb_wallet_balance(w, &tok_a);
        AMT_IS(&b, "1500000000000000000");
        b = tb_slot_available(s, &tok_a);
        AMT_IS(&b, "500000000000000000");
    }

    /* списание больше доступного — отказ, журнал не меняется */
    amt = dec("600000000000000000");
    CHECK(tb_slot_debit(s, &tok_a, &amt, &err) != 0);
    CHECK(err.code == TB_EXEC_INSUFFICIENT_BALANCE);
    {
        tb_u256_t b = tb_slot_available(s, &tok_a);
        AMT_IS(&b, "500000000000000000");
    }

    /* ---- инвариант receive(): кредит нативного запрещён ---- */
    amt = dec("1");
    CHECK(tb_slot_credit(s, &native, &amt, &err) != 0);
    CHECK(err.code == TB_EXEC_NOT_WRAPPED);

    /* ---- wrap/unwrap ---- */
    amt = dec("2000000000000000000");
    CHECK(tb_slot_wrap(s, &weth, &amt, &err) == 0);
    {
        tb_u256_t b = tb_slot_available(s, &native);
        AMT_IS(&b, "5000000000000000000");
        b = tb_slot_available(s, &weth);
        AMT_IS(&b, "2000000000000000000");
    }
    amt = dec("500000000000000000");
    CHECK(tb_slot_unwrap(s, &weth, &amt, &err) == 0);
    {
        tb_u256_t b = tb_slot_available(s, &weth);
        AMT_IS(&b, "1500000000000000000");
        b = tb_slot_available(s, &native);
        AMT_IS(&b, "5500000000000000000");
    }

    /* unwrap без wrapped — отказ */
    tb_slot_rollback(s);
    s = tb_slot_open(w, &err);
    CHECK(s != NULL);
    amt = dec("1");
    CHECK(tb_slot_unwrap(s, &weth, &amt, &err) != 0);
    CHECK(err.code == TB_EXEC_INSUFFICIENT_BALANCE);

    /* ---- allowance (forceApprove -> 0) ---- */
    tb_u256_t allow = dec("1234567890123456789");
    CHECK(tb_slot_approve(s, &tok_a, &allow, &err) == 0);
    {
        tb_u256_t a = tb_slot_allowance(s, &tok_a);
        AMT_IS(&a, "1234567890123456789");
    }
    tb_u256_t nul = tb_u256_from_u64(0);
    CHECK(tb_slot_approve(s, &tok_a, &nul, &err) == 0);
    {
        tb_u256_t a = tb_slot_allowance(s, &tok_a);
        CHECK(tb_u256_is_zero(&a));
    }

    /* ---- коммит применяет всё атомарно ---- */
    amt = dec("100000000000000000");
    CHECK(tb_slot_debit(s, &tok_a, &amt, &err) == 0);
    amt = dec("42");
    CHECK(tb_slot_credit(s, &tok_b, &amt, &err) == 0);
    CHECK(tb_slot_approve(s, &tok_b, &amt, &err) == 0);
    CHECK(tb_slot_commit(s, &err) == 0); /* слот закрыт и освобождён */

    {
        tb_u256_t b = tb_wallet_balance(w, &tok_a);
        AMT_IS(&b, "1400000000000000000");
        b = tb_wallet_balance(w, &tok_b);
        AMT_IS(&b, "42");
        tb_u256_t a = tb_wallet_allowance(w, &tok_b);
        AMT_IS(&a, "42");
        a = tb_wallet_allowance(w, &tok_a);
        CHECK(tb_u256_is_zero(&a)); /* approve(0) зафиксирован */
    }

    /* ---- rollback отбрасывает всё ---- */
    s = tb_slot_open(w, &err);
    amt = dec("1400000000000000000");
    CHECK(tb_slot_debit(s, &tok_a, &amt, &err) == 0);
    amt = dec("100000000000000000");
    CHECK(tb_slot_wrap(s, &weth, &amt, &err) == 0);
    tb_slot_rollback(s);

    {
        tb_u256_t b = tb_wallet_balance(w, &tok_a);
        AMT_IS(&b, "1400000000000000000");
        b = tb_wallet_balance(w, &native);
        AMT_IS(&b, "7000000000000000000");
        b = tb_wallet_balance(w, &weth);
        CHECK(tb_u256_is_zero(&b));
    }

    /* ---- wrap: нативного должно хватить ---- */
    s = tb_slot_open(w, &err);
    amt = dec("8000000000000000000");
    CHECK(tb_slot_wrap(s, &weth, &amt, &err) != 0);
    CHECK(err.code == TB_EXEC_INSUFFICIENT_BALANCE);
    amt = dec("7000000000000000000");
    CHECK(tb_slot_wrap(s, &weth, &amt, &err) == 0);
    CHECK(tb_slot_commit(s, &err) == 0);
    {
        tb_u256_t b = tb_wallet_balance(w, &native);
        AMT_IS(&b, "0");
        b = tb_wallet_balance(w, &weth);
        AMT_IS(&b, "7000000000000000000");
    }

    tb_wallet_destroy(w);
    printf("OK test_wallet\n");
    return 0;
}
