#define TB_CURRENT_GROUP TB_G_EXEC /* зона ответственности файла */
/* ==========================================================================
 *  tb/exec/u256.c — реализация 256-битной арифметики сумм.
 *
 *  Переносимый C: 64-битные слова с переносами; умножение и деление на
 *  64-битный множитель — через 128-битный промежуток там, где компилятор
 *  его даёт (x86-64/aarch64), иначе — разложение на 32-битные полусловья.
 * ========================================================================== */
#include "tb/exec/u256.h"

#include <string.h>

#if defined(__SIZEOF_INT128__)
    #define TB_U256_HAS_U128 1
#endif

int tb_u256_cmp(const tb_u256_t *a, const tb_u256_t *b)
{
    for (int i = 3; i >= 0; i--) {
        if (a->w[i] < b->w[i]) return -1;
        if (a->w[i] > b->w[i]) return 1;
    }
    return 0;
}

bool tb_u256_add(tb_u256_t *r, const tb_u256_t *a, const tb_u256_t *b)
{
    uint64_t carry = 0;
    uint64_t out[4];
    for (int i = 0; i < 4; i++) {
        uint64_t s = a->w[i] + b->w[i];
        uint64_t c1 = s < a->w[i];          /* перенос сложения */
        uint64_t t = s + carry;
        uint64_t c2 = t < s;                /* перенос от переноса */
        out[i] = t;
        carry = c1 | c2;
    }
    if (carry)
        return false;
    memcpy(r->w, out, sizeof(out));
    return true;
}

bool tb_u256_sub(tb_u256_t *r, const tb_u256_t *a, const tb_u256_t *b)
{
    uint64_t borrow = 0;
    uint64_t out[4];
    for (int i = 0; i < 4; i++) {
        uint64_t d = a->w[i] - b->w[i];
        uint64_t b1 = a->w[i] < b->w[i];    /* заём вычитания */
        uint64_t t = d - borrow;
        uint64_t b2 = d < borrow;           /* заём от заёма */
        out[i] = t;
        borrow = b1 | b2;
    }
    if (borrow)
        return false;
    memcpy(r->w, out, sizeof(out));
    return true;
}

bool tb_u256_mul_u64(tb_u256_t *r, const tb_u256_t *a, uint64_t m)
{
    if (m == 0) {
        memset(r->w, 0, sizeof(r->w));
        return true;
    }

    uint64_t out[4];
    uint64_t carry = 0;
#ifdef TB_U256_HAS_U128
    for (int i = 0; i < 4; i++) {
        unsigned __int128 p = (unsigned __int128)a->w[i] * m + carry;
        out[i] = (uint64_t)p;
        carry  = (uint64_t)(p >> 64);
    }
#else
    /* Переносимый путь: слово = (ah:al) * (mh:ml) + перенос.
     * произведение = p0 + (p1<<32) + (p2<<32) + (p3<<64), где
     * p0 = al*ml, p1 = al*mh, p2 = ah*ml, p3 = ah*mh — все в 64 битах. */
    const uint32_t ml = (uint32_t)m, mh = (uint32_t)(m >> 32);
    for (int i = 0; i < 4; i++) {
        const uint32_t al = (uint32_t)a->w[i], ah = (uint32_t)(a->w[i] >> 32);

        const uint64_t p1s = (uint64_t)al * mh << 32; /* младшая часть p1 */
        const uint64_t p2s = (uint64_t)ah * ml << 32; /* младшая часть p2 */
        const uint64_t p0  = (uint64_t)al * ml;
        const uint64_t p3  = (uint64_t)ah * mh;
        const uint64_t p1h = (uint64_t)al * mh >> 32; /* старшая часть p1 */
        const uint64_t p2h = (uint64_t)ah * ml >> 32; /* старшая часть p2 */

        uint64_t s1 = p0 + p1s;        const uint64_t c1 = s1 < p0;
        uint64_t s2 = s1 + p2s;        const uint64_t c2 = s2 < s1;
        uint64_t lo = s2 + carry;      const uint64_t c3 = lo < s2;

        uint64_t h1 = p3 + p1h;        const uint64_t c4 = h1 < p3;
        uint64_t h2 = h1 + p2h;        const uint64_t c5 = h2 < h1;
        uint64_t hi = h2 + c1 + c2 + c3 + c4 + c5; /* переполнение здесь = переполнение 256 */
        if (hi < h2)
            return false;
        out[i] = lo;
        carry  = hi;
    }
#endif
    if (carry)
        return false;
    memcpy(r->w, out, sizeof(out));
    return true;
}

uint64_t tb_u256_div_u64(tb_u256_t *q, const tb_u256_t *a, uint64_t d)
{
    if (d == 0)
        tb_panic(TB_LOC, "tb_u256_div_u64: деление на ноль");

    if (d == 1) {
        memcpy(q->w, a->w, sizeof(q->w));
        return 0;
    }

    uint64_t out[4];
    uint64_t rem;
#ifdef TB_U256_HAS_U128
    rem = 0;
    for (int i = 3; i >= 0; i--) {
        unsigned __int128 cur = ((unsigned __int128)rem << 64) | a->w[i];
        out[i] = (uint64_t)(cur / d);
        rem    = (uint64_t)(cur % d);
    }
#else
    /* Битовое восстановительное деление. Так как d <= 2^63 (иначе паника
     * ниже), остаток всегда < 2^63 и (rem<<1)|bit не переполняет 64 бита. */
    if (d > (UINT64_C(1) << 63))
        tb_panic(TB_LOC,
                 "tb_u256_div_u64: переносимый путь не поддерживает d > 2^63");
    memset(out, 0, sizeof(out));
    rem = 0;
    for (int i = 255; i >= 0; i--) {
        const int word = i >> 6, bit = i & 63;
        const uint64_t bitval = (a->w[word] >> bit) & 1u;
        rem = (rem << 1) | bitval;
        if (rem >= d) {
            rem -= d;
            out[word] |= (uint64_t)1 << bit;
        }
    }
#endif
    memcpy(q->w, out, sizeof(out));
    return rem;
}

int tb_u256_from_dec(tb_u256_t *out, const char *s, size_t len, tb_error_t *err)
{
    if (!s || len == 0 || len > 78) {
        TB_ERROR_SET(err, TB_DOMAIN_USER, TB_EXEC_BAD_DECIMALS,
                     "сумма: ожидалась десятичная строка из 1..78 цифр");
        return -1;
    }
    tb_u256_t v = tb_u256_from_u64(0);
    for (size_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') {
            TB_ERROR_SET(err, TB_DOMAIN_USER, TB_EXEC_BAD_DECIMALS,
                         "сумма: недопустимый символ '%c' на позиции %zu",
                         s[i], i);
            return -1;
        }
        tb_u256_t t, d;
        d = tb_u256_from_u64((uint64_t)(s[i] - '0'));
        if (!tb_u256_mul_u64(&t, &v, 10) || !tb_u256_add(&v, &t, &d)) {
            TB_ERROR_SET(err, TB_DOMAIN_USER, TB_EXEC_OVERFLOW,
                         "сумма: переполнение uint256 при разборе (позиция %zu)", i);
            return -1;
        }
    }
    *out = v;
    return 0;
}

size_t tb_u256_to_dec(const tb_u256_t *a, char *buf, size_t cap)
{
    char tmp[TB_U256_DEC_MAX];
    size_t n = 0;
    if (tb_u256_is_zero(a)) {
        tmp[n++] = '0';
    } else {
        tb_u256_t v = *a;
        while (!tb_u256_is_zero(&v)) {
            uint64_t rem = tb_u256_div_u64(&v, &v, 10);
            tmp[n++] = (char)('0' + (rem % 10));
        }
    }
    if (buf && cap > 0) {
        size_t copy = n < cap - 1 ? n : cap - 1;
        for (size_t i = 0; i < copy; i++)
            buf[i] = tmp[n - 1 - i];
        buf[copy] = '\0';
    }
    return n;
}
