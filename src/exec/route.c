#define TB_CURRENT_GROUP TB_G_EXEC /* зона ответственности файла */
/* ==========================================================================
 *  tb/exec/route.c — адреса и маршруты Uniswap V3 (упаковка/разбор).
 * ========================================================================== */
#include "tb/exec/route.h"

#include <ctype.h>
#include <string.h>

/* ======================================================================
 *  Адреса
 * ====================================================================== */
static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int tb_addr_from_hex(tb_addr_t *out, const char *hex, tb_error_t *err)
{
    if (!out || !hex) {
        TB_ERROR_SET(err, TB_DOMAIN_CORE, TB_CORE_INVALID_ARG,
                     "адрес: пустой аргумент");
        return -1;
    }
    if (hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X'))
        hex += 2;

    size_t n = strlen(hex);
    if (n != 40) {
        TB_ERROR_SET(err, TB_DOMAIN_USER, TB_EXEC_INVALID_PATH,
                     "адрес: ожидалось 40 hex-символов, получено %zu", n);
        return -1;
    }
    for (size_t i = 0; i < 20; i++) {
        int hi = hexval(hex[2 * i]);
        int lo = hexval(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            TB_ERROR_SET(err, TB_DOMAIN_USER, TB_EXEC_INVALID_PATH,
                         "адрес: недопустимый hex-символ на позиции %zu", 2 * i);
            return -1;
        }
        out->b[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

size_t tb_addr_to_hex(const tb_addr_t *a, char *buf, size_t cap)
{
    static const char digits[] = "0123456789abcdef";
    if (!buf || cap < 43)
        return 43;
    buf[0] = '0';
    buf[1] = 'x';
    for (size_t i = 0; i < 20; i++) {
        buf[2 + 2 * i]     = digits[a->b[i] >> 4];
        buf[2 + 2 * i + 1] = digits[a->b[i] & 0xF];
    }
    buf[42] = '\0';
    return 43;
}

/* ======================================================================
 *  Маршрут
 * ====================================================================== */
tb_route_t tb_route_encode(const tb_addr_t *tokens, const uint32_t *fees,
                           size_t ntokens, tb_error_t *err)
{
    tb_route_t r;
    memset(&r, 0, sizeof(r));

    if (!tokens || !fees || ntokens < 2) {
        TB_ERROR_SET(err, TB_DOMAIN_USER, TB_EXEC_INVALID_PATH,
                     "маршрут: нужно минимум 2 токена");
        return r;
    }
    if (ntokens - 1 > TB_EXEC_MAX_HOPS) {
        TB_ERROR_SET(err, TB_DOMAIN_USER, TB_EXEC_INVALID_PATH,
                     "маршрут: %zu хопов больше лимита %d",
                     ntokens - 1, TB_EXEC_MAX_HOPS);
        return r;
    }

    r.len = 0;
    memcpy(r.bytes + r.len, tokens[0].b, 20);
    r.len += 20;
    for (size_t i = 0; i + 1 < ntokens; i++) {
        if (fees[i] >= TB_ROUTE_FEE_MAX) {
            TB_ERROR_SET(err, TB_DOMAIN_USER, TB_EXEC_INVALID_PATH,
                         "маршрут: fee %u хопа %zu вне диапазона V3 (< %u)",
                         fees[i], i, TB_ROUTE_FEE_MAX);
            memset(&r, 0, sizeof(r));
            return r;
        }
        r.bytes[r.len++] = (uint8_t)(fees[i] >> 16);
        r.bytes[r.len++] = (uint8_t)(fees[i] >> 8);
        r.bytes[r.len++] = (uint8_t)(fees[i]);
        memcpy(r.bytes + r.len, tokens[i + 1].b, 20);
        r.len += 20;
    }
    return r;
}

tb_route_t tb_route_single(const tb_addr_t *a, const tb_addr_t *b,
                           uint32_t fee, tb_error_t *err)
{
    const tb_addr_t tokens[2] = { *a, *b };
    const uint32_t fees[1] = { fee };
    return tb_route_encode(tokens, fees, 2, err);
}

bool tb_route_valid(const tb_route_t *r)
{
    if (!r || r->len < TB_ROUTE_MIN_LEN || r->len > TB_ROUTE_MAX_LEN)
        return false;
    if ((r->len - 20) % 23 != 0)
        return false;
    for (size_t hop = 0; hop < tb_route_hops(r); hop++)
        if (tb_route_fee(r, hop) >= TB_ROUTE_FEE_MAX)
            return false;
    return true;
}

size_t tb_route_hops(const tb_route_t *r)
{
    if (!r || r->len < TB_ROUTE_MIN_LEN || (r->len - 20) % 23 != 0)
        return 0;
    return (r->len - 20) / 23;
}

tb_addr_t tb_route_token(const tb_route_t *r, size_t i)
{
    tb_addr_t a = tb_addr_zero();
    size_t hops = tb_route_hops(r);
    if (!r || i > hops)
        return a;
    size_t off = i * 23; /* 20 байт токена + 3 байта fee перед каждым следующим */
    memcpy(a.b, r->bytes + off, 20);
    return a;
}

uint32_t tb_route_fee(const tb_route_t *r, size_t hop)
{
    size_t hops = tb_route_hops(r);
    if (!r || hop >= hops)
        return 0;
    size_t off = 20 + hop * 23;
    return ((uint32_t)r->bytes[off] << 16) |
           ((uint32_t)r->bytes[off + 1] << 8) |
           (uint32_t)r->bytes[off + 2];
}

size_t tb_route_hex(const tb_route_t *r, char *buf, size_t cap)
{
    static const char digits[] = "0123456789abcdef";
    size_t need = (size_t)r->len * 2 + 3; /* "0x" + hex + '\0' */
    if (!buf || cap < need || !r)
        return need;
    buf[0] = '0';
    buf[1] = 'x';
    for (size_t i = 0; i < r->len; i++) {
        buf[2 + 2 * i]     = digits[r->bytes[i] >> 4];
        buf[2 + 2 * i + 1] = digits[r->bytes[i] & 0xF];
    }
    buf[2 + (size_t)r->len * 2] = '\0';
    return need;
}
