/* tests/test_route.c — адреса и маршруты Uniswap V3. */
#include "harness.h"
#include "tb/exec/route.h"

#include <string.h>

int main(void)
{
    tb_error_t err = TB_ERROR_INIT;

    /* Помощники: доступ по значению -> сравнение по адресу локала. */
    #define ROUTE_TOKEN_IS(rp, i, want) \
        ({ tb_addr_t t_ = tb_route_token((rp), (i)); tb_addr_eq(&t_, (want)); })
    #define ROUTE_FIRST_IS(rp, want) \
        ({ tb_addr_t t_ = tb_route_first((rp)); tb_addr_eq(&t_, (want)); })
    #define ROUTE_LAST_IS(rp, want) \
        ({ tb_addr_t t_ = tb_route_last((rp)); tb_addr_eq(&t_, (want)); })

    /* ---- адреса ---- */
    {
        tb_addr_t a;
        CHECK(tb_addr_from_hex(&a, "0x1111111111111111111111111111111111111111",
                               &err) == 0);
        char hex[43];
        tb_addr_to_hex(&a, hex, sizeof(hex));
        CHECK_STR_EQ(hex, "0x1111111111111111111111111111111111111111");

        /* без 0x-префикса */
        CHECK(tb_addr_from_hex(&a, "2222222222222222222222222222222222222222",
                               &err) == 0);
        tb_addr_to_hex(&a, hex, sizeof(hex));
        CHECK_STR_EQ(hex, "0x2222222222222222222222222222222222222222");

        /* верхний регистр — допустим, вывод — нижний */
        CHECK(tb_addr_from_hex(&a, "0xAbCdEfAbCdEfAbCdEfAbCdEfAbCdEfAbCdEfAbCd",
                               &err) == 0);
        tb_addr_to_hex(&a, hex, sizeof(hex));
        CHECK_STR_EQ(hex, "0xabcdefabcdefabcdefabcdefabcdefabcdefabcd");

        /* NATIVE-сентинел */
        tb_addr_t z = tb_addr_zero();
        CHECK(tb_addr_is_native(&z));
        CHECK(!tb_addr_is_native(&a));
        tb_addr_t z2;
        CHECK(tb_addr_from_hex(&z2, "0x0000000000000000000000000000000000000000",
                               &err) == 0);
        CHECK(tb_addr_eq(&z, &z2));

        /* ошибки длины и символов */
        CHECK(tb_addr_from_hex(&a, "0x123", &err) != 0);
        CHECK(err.code == TB_EXEC_INVALID_PATH);
        CHECK(tb_addr_from_hex(&a, "0xZZ11111111111111111111111111111111111111",
                               &err) != 0);
    }

    /* ---- сборка маршрута: однохоповый (43 байта) ---- */
    {
        tb_error_clear(&err);
        tb_addr_t a, b;
        tb_addr_from_hex(&a, "0xaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", NULL);
        tb_addr_from_hex(&b, "0xbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", NULL);

        tb_route_t r = tb_route_single(&a, &b, 3000, &err);
        CHECK(err.code == 0);
        CHECK(r.len == 43);
        CHECK(tb_route_valid(&r));
        CHECK(tb_route_hops(&r) == 1);
        CHECK(ROUTE_FIRST_IS(&r, &a));
        CHECK(ROUTE_LAST_IS(&r, &b));
        CHECK(tb_route_fee(&r, 0) == 3000);
        CHECK(tb_route_fee(&r, 1) == 0); /* вне диапазона — 0 */

        /* Первый байт fee = 0x00, затем 0x0b, 0xb8 (3000 = 0x0BB8). */
        CHECK(r.bytes[20] == 0x00 && r.bytes[21] == 0x0b && r.bytes[22] == 0xb8);
        CHECK(r.bytes[23] == 0xbb); /* начало второго токена */
    }

    /* ---- многохоповый: A -500- B -3000- C ---- */
    {
        tb_error_clear(&err);
        tb_addr_t a, b, c;
        tb_addr_from_hex(&a, "0xaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", NULL);
        tb_addr_from_hex(&b, "0xbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", NULL);
        tb_addr_from_hex(&c, "0xcccccccccccccccccccccccccccccccccccccccc", NULL);

        tb_addr_t toks[3] = { a, b, c };
        uint32_t fees[2]  = { 500, 3000 };
        tb_route_t r = tb_route_encode(toks, fees, 3, &err);
        CHECK(err.code == 0);
        CHECK(r.len == 66); /* 20 + 23 + 23 */
        CHECK(tb_route_valid(&r));
        CHECK(tb_route_hops(&r) == 2);
        CHECK(ROUTE_TOKEN_IS(&r, 0, &a));
        CHECK(ROUTE_TOKEN_IS(&r, 1, &b));
        CHECK(ROUTE_TOKEN_IS(&r, 2, &c));
        CHECK(ROUTE_LAST_IS(&r, &c));
        CHECK(tb_route_fee(&r, 0) == 500);
        CHECK(tb_route_fee(&r, 1) == 3000);

        /* hex-представление: длина 2*66+2 без нуля */
        char hex[200];
        size_t need = tb_route_hex(&r, hex, sizeof(hex));
        CHECK(need == 2 * 66 + 3);
        CHECK(hex[0] == '0' && hex[1] == 'x');
        CHECK(strlen(hex) == 2 * 66 + 2);
    }

    /* ---- лимит хопов и ошибки сборки ---- */
    {
        tb_error_clear(&err);
        tb_addr_t toks[TB_EXEC_MAX_HOPS + 2];
        uint32_t fees[TB_EXEC_MAX_HOPS + 1];
        for (size_t i = 0; i < TB_EXEC_MAX_HOPS + 2; i++)
            tb_addr_from_hex(&toks[i], "0xaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", NULL);
        for (size_t i = 0; i < TB_EXEC_MAX_HOPS + 1; i++)
            fees[i] = 3000;

        tb_route_t r = tb_route_encode(toks, fees, TB_EXEC_MAX_HOPS + 2, &err);
        CHECK(err.code == TB_EXEC_INVALID_PATH); /* 9 хопов > 8 */
        err.code = 0;

        /* ровно лимит — собирается */
        r = tb_route_encode(toks, fees, TB_EXEC_MAX_HOPS + 1, &err);
        CHECK(err.code == 0);
        CHECK(tb_route_valid(&r));
        CHECK(tb_route_hops(&r) == TB_EXEC_MAX_HOPS);

        /* fee вне диапазона */
        uint32_t bad[1] = { TB_ROUTE_FEE_MAX };
        tb_addr_t two[2] = { toks[0], toks[1] };
        r = tb_route_encode(two, bad, 2, &err);
        CHECK(err.code == TB_EXEC_INVALID_PATH);
        CHECK(!tb_route_valid(&r));

        /* один токен — не маршрут */
        r = tb_route_encode(toks, fees, 1, &err);
        CHECK(err.code == TB_EXEC_INVALID_PATH);
    }

    /* ---- структурная валидность ---- */
    {
        tb_route_t r;
        memset(&r, 0, sizeof(r));

        r.len = 42; /* короче минимального */
        CHECK(!tb_route_valid(&r));

        r.len = 44; /* не кратно структуре 20+23k */
        CHECK(!tb_route_valid(&r));
        CHECK(tb_route_hops(&r) == 0);

        r.len = 43;
        memset(r.bytes, 0x11, 43);
        r.bytes[20] = 0x0f; /* fee = 0x0f4240 = 1000000 — вне диапазона */
        r.bytes[21] = 0x42;
        r.bytes[22] = 0x40;
        CHECK(!tb_route_valid(&r));

        r.bytes[20] = 0x00; r.bytes[21] = 0x0b; r.bytes[22] = 0xb8; /* 3000 */
        CHECK(tb_route_valid(&r));
    }

    /* Пулы хопов (v0.3): дефолт — нулевые, установка/чтение, выход за
     * диапазон — нулевой адрес. */
    {
        tb_error_clear(&err);
        tb_addr_t pa, pb, pool;
        CHECK(tb_addr_from_hex(&pa, "0xaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                               &err) == 0);
        CHECK(tb_addr_from_hex(&pb, "0xbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
                               &err) == 0);
        CHECK(tb_addr_from_hex(&pool, "0x1111111111111111111111111111111111111111",
                               &err) == 0);
        tb_route_t r = tb_route_single(&pa, &pb, 3000, &err);
        tb_addr_t p0;
        CHECK(tb_route_valid(&r));
        p0 = tb_route_pool(&r, 0);
        CHECK(tb_addr_is_native(&p0)); /* не задан */

        tb_route_set_pool(&r, 0, &pool);
        p0 = tb_route_pool(&r, 0);
        CHECK(tb_addr_eq(&p0, &pool));

        tb_route_t copy = r; /* POD-копирование переносит пулы */
        p0 = tb_route_pool(&copy, 0);
        CHECK(tb_addr_eq(&p0, &pool));

        p0 = tb_route_pool(&r, 1);
        CHECK(tb_addr_is_native(&p0)); /* вне маршрута */
        tb_route_set_pool(&r, TB_EXEC_MAX_HOPS, &pool); /* вне массива — игнор */
        p0 = tb_route_pool(&r, TB_EXEC_MAX_HOPS);
        CHECK(tb_addr_is_native(&p0));
    }

    printf("OK test_route\n");
    return 0;
}
