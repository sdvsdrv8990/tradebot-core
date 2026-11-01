/* ==========================================================================
 *  tb/exec/u256.h — 256-битные суммы (аналог uint256 контрактов).
 *
 *  Зачем не uint64: суммы в базовых единицах токенов с 18 знаками
 *  (5M токенов = 5*10^24 wei) не помещаются в 64 бита. JSON-граница:
 *  суммы сериализуются ДЕСЯТИЧНЫМИ СТРОКАМИ (стандартная практика
 *  JSON-API к uint256 — без потери точности).
 *
 *  Стиль ядра: все операции с проверкой переполнения; слово little-endian;
 *  значение — POD, копируется присваиванием.
 * ========================================================================== */
#ifndef TB_EXEC_U256_H
#define TB_EXEC_U256_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "tb/core/error.h"
#include "tb/exec/codes.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TB_U256_DEC_MAX 79 /* 2^256-1 = 78 цифр + завершающий ноль */

typedef struct tb_u256 {
    uint64_t w[4]; /* младшее слово — w[0] */
} tb_u256_t;

static inline tb_u256_t tb_u256_from_u64(uint64_t v)
{
    tb_u256_t r = { { v, 0, 0, 0 } };
    return r;
}

static inline bool tb_u256_is_zero(const tb_u256_t *a)
{
    return a->w[0] == 0 && a->w[1] == 0 && a->w[2] == 0 && a->w[3] == 0;
}

/* -1 / 0 / +1 */
int tb_u256_cmp(const tb_u256_t *a, const tb_u256_t *b);

/* r = a + b. false — переполнение (r не трогается). */
bool tb_u256_add(tb_u256_t *r, const tb_u256_t *a, const tb_u256_t *b);

/* r = a - b. false — отрицательный результат (r не трогается). */
bool tb_u256_sub(tb_u256_t *r, const tb_u256_t *a, const tb_u256_t *b);

/* r = a * m. false — переполнение (r не трогается). */
bool tb_u256_mul_u64(tb_u256_t *r, const tb_u256_t *a, uint64_t m);

/* q = a / d, возврат — остаток. Контракт: d != 0 (при d == 0 — паника
 * ядра: деление на ноль в арифметике сумм недопустимо по построению). */
uint64_t tb_u256_div_u64(tb_u256_t *q, const tb_u256_t *a, uint64_t d);

/* Разбор десятичной строки (только цифры, длина 1..78). 0 — успех. */
int tb_u256_from_dec(tb_u256_t *out, const char *s, size_t len, tb_error_t *err);

/* Формат в десятичную строку. Возврат — требуемая длина БЕЗ нуля;
 * записывается не более cap-1 символа + '\0'. buf/cap == 0 — только длину. */
size_t tb_u256_to_dec(const tb_u256_t *a, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
#endif /* TB_EXEC_U256_H */
