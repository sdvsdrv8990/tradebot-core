/* ==========================================================================
 *  tb/exec/route.h — адреса активов и маршруты Uniswap V3.
 *
 *  Перенос семантики контракта-референса:
 *    encodePath(tokens, fees) -> tb_route_encode()
 *    path[0:20] (_firstToken) -> tb_route_first()
 *    InvalidPath (len < 43)   -> tb_route_valid()
 *
 *  Путь — плотно упакованные байты: token(20) | fee(3) | token(20) | ...,
 *  как в SwapRouter.exactInput. Первый токен — вход (или wrapped, если
 *  платим нативным), последний — выход (или wrapped, если получаем
 *  нативный). NATIVE-сентинел — нулевой адрес, как address(0) в контракте.
 *
 *  Маршрут — POD с фиксированным буфером (стиль ядра): memcpy-безопасен,
 *  переполнение исключено лимитом хопов, усечения нет — длинный маршрут
 *  отвергается ошибкой, а не обрезается.
 * ========================================================================== */
#ifndef TB_EXEC_ROUTE_H
#define TB_EXEC_ROUTE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "tb/core/error.h"
#include "tb/exec/codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Адрес актива (20 байт, как address в EVM) ---- */
typedef struct tb_addr {
    uint8_t b[20];
} tb_addr_t;

/* NATIVE-сентинел: нулевой адрес означает нативный актив (ETH). */
static inline tb_addr_t tb_addr_zero(void)
{
    tb_addr_t a = { { 0 } };
    return a;
}
static inline bool tb_addr_is_native(const tb_addr_t *a)
{
    for (size_t i = 0; i < 20; i++)
        if (a->b[i])
            return false;
    return true;
}
static inline bool tb_addr_eq(const tb_addr_t *a, const tb_addr_t *b)
{
    for (size_t i = 0; i < 20; i++)
        if (a->b[i] != b->b[i])
            return false;
    return true;
}

/* "0x" + 40 hex (или 40 hex без префикса). 0 — успех. */
int tb_addr_from_hex(tb_addr_t *out, const char *hex, tb_error_t *err);

/* Строка "0x" + 40 символов hex (нижний регистр) + '\0'. Возврат — требуемая
 * число байт с нулём (43); при cap меньше — усечение по усмотрению вызова. */
size_t tb_addr_to_hex(const tb_addr_t *a, char *buf, size_t cap);

/* ---- Маршрут ---- */
#define TB_EXEC_MAX_HOPS  8                        /* лимит хопов механизма */
#define TB_ROUTE_MIN_LEN  43                       /* 20 + 3 + 20 */
#define TB_ROUTE_MAX_LEN  (20 + 23 * TB_EXEC_MAX_HOPS)
#define TB_ROUTE_FEE_MAX  1000000u                 /* fee < 1e6 (100%): диапазон тиров V3 */

typedef struct tb_route {
    uint8_t  bytes[TB_ROUTE_MAX_LEN];
    uint32_t len;
    /* Пул каждого хопа (v0.3: биржа/venue исполнения хопа — пул DEX).
     * Нулевой адрес = пул не задан (симулятор игнорирует; живой
     * DEX-адаптер требует явного пула). Аддитивное поле: маршруты,
     * собранные tb_route_encode(), имеют нулевые пулы. */
    tb_addr_t pools[TB_EXEC_MAX_HOPS];
} tb_route_t;

/* Сборка маршрута из токенов и фи (fees[n-1] для n токенов, n >= 2).
 * Аналог encodePath() из контракта — та же укладка, тот же порядок. */
tb_route_t tb_route_encode(const tb_addr_t *tokens, const uint32_t *fees,
                           size_t ntokens, tb_error_t *err);

/* Сахар для однохопового маршрута (аналог exactInputSingle). */
tb_route_t tb_route_single(const tb_addr_t *a, const tb_addr_t *b,
                           uint32_t fee, tb_error_t *err);

/* Структурная валидность: len >= 43, (len - 20) кратно 23, fee-тиры в диапазоне. */
bool tb_route_valid(const tb_route_t *r);

/* Число хопов (0, если маршрут невалиден). */
size_t tb_route_hops(const tb_route_t *r);

/* Токен с индексом i (0..hops); fee хопа i (0..hops-1). Для невалидного
 * маршрута — нули. */
tb_addr_t tb_route_token(const tb_route_t *r, size_t i);
uint32_t  tb_route_fee(const tb_route_t *r, size_t hop);

static inline tb_addr_t tb_route_first(const tb_route_t *r)
{
    return tb_route_token(r, 0);
}
static inline tb_addr_t tb_route_last(const tb_route_t *r)
{
    return tb_route_token(r, tb_route_hops(r));
}

/* Пул хопа hop (0..hops-1). */
static inline tb_addr_t tb_route_pool(const tb_route_t *r, size_t hop)
{
    if (hop >= TB_EXEC_MAX_HOPS)
        return tb_addr_zero();
    return r->pools[hop];
}

static inline void tb_route_set_pool(tb_route_t *r, size_t hop,
                                     const tb_addr_t *pool)
{
    if (hop < TB_EXEC_MAX_HOPS && r && pool)
        r->pools[hop] = *pool;
}

/* Hex всего пути (для следов и журнала). Возврат — требуемая длина с '\0'. */
size_t tb_route_hex(const tb_route_t *r, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
#endif /* TB_EXEC_ROUTE_H */
