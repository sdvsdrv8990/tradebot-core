/* tests/test_u256.c — 256-битная арифметика сумм. */
#include "harness.h"
#include "tb/exec/u256.h"

#include <inttypes.h>
#include <string.h>

static tb_u256_t u64s(uint64_t hi, uint64_t lo) /* 128-битное значение для удобства */
{
    tb_u256_t r = { { lo, hi, 0, 0 } };
    return r;
}

int main(void)
{
    tb_error_t err = TB_ERROR_INIT;

    /* ---- десятичный roundtrip ---- */
    {
        const char *big = "123456789012345678901234567890123456789";
        tb_u256_t v;
        CHECK(tb_u256_from_dec(&v, big, strlen(big), &err) == 0);
        char buf[TB_U256_DEC_MAX];
        tb_u256_to_dec(&v, buf, sizeof(buf));
        CHECK_STR_EQ(buf, big);
    }
    {
        tb_u256_t v;
        CHECK(tb_u256_from_dec(&v, "0", 1, &err) == 0);
        CHECK(tb_u256_is_zero(&v));
        char buf[8];
        CHECK(tb_u256_to_dec(&v, buf, sizeof(buf)) == 1);
        CHECK_STR_EQ(buf, "0");
    }
    {
        /* 2^128 = 340282366920938463463374607431768211456 (слово w2) */
        tb_u256_t v = { { 0, 0, 1, 0 } };
        char buf[TB_U256_DEC_MAX];
        tb_u256_to_dec(&v, buf, sizeof(buf));
        CHECK_STR_EQ(buf, "340282366920938463463374607431768211456");
    }

    /* ---- разбор: ошибки ---- */
    {
        tb_u256_t v;
        CHECK(tb_u256_from_dec(&v, "12x3", 4, &err) != 0);
        CHECK(err.code == TB_EXEC_BAD_DECIMALS);
        CHECK(tb_u256_from_dec(&v, "", 0, &err) != 0);
        CHECK(tb_u256_from_dec(&v,
               "115792089237316195423570985008687907853269984665640564039457584007913129639936",
               78, &err) != 0); /* 2^256 — не влезает */
        CHECK(err.code == TB_EXEC_OVERFLOW);
    }

    /* ---- сравнения ---- */
    {
        tb_u256_t a = tb_u256_from_u64(100), b = tb_u256_from_u64(100),
                  c = tb_u256_from_u64(101);
        CHECK(tb_u256_cmp(&a, &b) == 0);
        CHECK(tb_u256_cmp(&a, &c) < 0);
        CHECK(tb_u256_cmp(&c, &a) > 0);
        tb_u256_t hi = u64s(1, 0); /* 2^64 */
        CHECK(tb_u256_cmp(&hi, &c) > 0);
    }

    /* ---- сложение и перенос ---- */
    {
        tb_u256_t a = u64s(0, UINT64_MAX), b = tb_u256_from_u64(1);
        tb_u256_t s;
        CHECK(tb_u256_add(&s, &a, &b));
        CHECK(s.w[0] == 0 && s.w[1] == 1 && s.w[2] == 0 && s.w[3] == 0);
        tb_u256_t max = { { UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX } };
        tb_u256_t one = tb_u256_from_u64(1);
        tb_u256_t nul = tb_u256_from_u64(0);
        CHECK(!tb_u256_add(&s, &max, &one)); /* переполнение */
        CHECK(tb_u256_add(&s, &max, &nul)); /* +0 — ок */
        CHECK(tb_u256_cmp(&s, &max) == 0);
    }

    /* ---- вычитание и заём ---- */
    {
        tb_u256_t a = u64s(1, 0), b = tb_u256_from_u64(1);
        tb_u256_t d;
        CHECK(tb_u256_sub(&d, &a, &b));
        CHECK(d.w[0] == UINT64_MAX && d.w[1] == 0);
        CHECK(!tb_u256_sub(&d, &b, &a)); /* отрицательный результат */
        tb_u256_t zero = tb_u256_from_u64(0);
        CHECK(tb_u256_sub(&d, &zero, &zero));
        CHECK(tb_u256_is_zero(&d));
    }

    /* ---- умножение на u64 ---- */
    {
        tb_u256_t a = u64s(0, UINT64_MAX); /* 2^64 - 1 */
        tb_u256_t m;
        CHECK(tb_u256_mul_u64(&m, &a, 2));
        /* (2^64-1)*2 = 2^65 - 2 = w0=UINT64_MAX-1, w1=1 */
        CHECK(m.w[0] == UINT64_MAX - 1 && m.w[1] == 1);

        tb_u256_t big = u64s(UINT64_MAX, UINT64_MAX); /* 2^128 - 1 */
        /* (2^128-1)*(2^64-1) — 192-битное: w3=0 w2=..FE w1=..FF w0=1 */
        CHECK(tb_u256_mul_u64(&m, &big, UINT64_MAX));
        CHECK(m.w[3] == 0 && m.w[2] == 0xFFFFFFFFFFFFFFFEull &&
              m.w[1] == UINT64_MAX && m.w[0] == 1);

        tb_u256_t max = { { UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX } };
        CHECK(!tb_u256_mul_u64(&m, &max, 2)); /* 2^256-1 умножить на 2 — не влезает */
        CHECK(tb_u256_mul_u64(&m, &big, 0));
        CHECK(tb_u256_is_zero(&m));
    }

    /* ---- деление на u64 ---- */
    {
        tb_u256_t q;
        /* 10^19 / 7: 10^19 = 10000000000000000000 */
        tb_u256_t v;
        CHECK(tb_u256_from_dec(&v, "10000000000000000000", 20, &err) == 0);
        uint64_t r = tb_u256_div_u64(&q, &v, 7);
        CHECK(r == 10000000000000000000ULL % 7);
        char buf[TB_U256_DEC_MAX];
        tb_u256_to_dec(&q, buf, sizeof(buf));
        char expect[32];
        snprintf(expect, sizeof(expect), "%llu",
                 (unsigned long long)(10000000000000000000ULL / 7));
        CHECK_STR_EQ(buf, expect);

        /* Деление с остатком через цепочку (имитация to_dec) — 12345 % 10... */
        tb_u256_t n;
        CHECK(tb_u256_from_dec(&n, "987654321987654321", 18, &err) == 0);
        uint64_t rem = tb_u256_div_u64(&q, &n, 1000000000ULL);
        CHECK(rem == 987654321987654321ULL % 1000000000ULL);
        tb_u256_to_dec(&q, buf, sizeof(buf));
        CHECK_STR_EQ(buf, "987654321");
    }

    /* ---- связка mul/div: масштабирование котировки ---- */
    {
        /* quote * 995 / 1000 без потери точности на больших числах */
        tb_u256_t q;
        CHECK(tb_u256_from_dec(&q, "123456789012345678901234567890", 30,
                               &err) == 0);
        tb_u256_t t;
        CHECK(tb_u256_mul_u64(&t, &q, 995));
        tb_u256_div_u64(&t, &t, 1000);
        char buf[TB_U256_DEC_MAX];
        tb_u256_to_dec(&t, buf, sizeof(buf));
        CHECK_STR_EQ(buf, "122839505067283950506728395050"); /* python-сверено */
    }

    printf("OK test_u256\n");
    return 0;
}
