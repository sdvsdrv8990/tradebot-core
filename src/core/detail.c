/* ==========================================================================
 *  tb/core/detail.c — встроенные экспандеры капсул деталей.
 *
 *  Формат компактный и адресный: каждая строка отвечает на вопрос
 *  «что именно пришло на границу модуля», чтобы не гадать по коду.
 * ========================================================================== */
#include "tb/core/detail.h"
#include "tb/core/error.h"

#include <stdio.h>
#include <string.h>

/* ---- упаковка (продюсеры) ---- */

size_t tb_detail_pack_net_raw(uint8_t *out, size_t cap, int32_t status,
                              const char *body, size_t body_len)
{
    if (cap < TB_DETAIL_DATA_MAX)
        return 0;
    memset(out, 0, TB_DETAIL_DATA_MAX);
    out[0] = (uint8_t)(status & 0xff);
    out[1] = (uint8_t)((status >> 8) & 0xff);
    out[2] = (uint8_t)((status >> 16) & 0xff);
    out[3] = (uint8_t)((status >> 24) & 0xff);
    size_t n = body_len;
    if (n > TB_DETAIL_DATA_MAX - 4)
        n = TB_DETAIL_DATA_MAX - 4;
    if (body && n)
        memcpy(out + 4, body, n);
    return 4 + n;
}

static void hex_head(uint8_t *dst, const char *hex, size_t digits)
{
    /* hex без "0x": первые digits символов -> байты (усечение слева
     * не теряет хвост — берём голову строки после 0x) */
    if (!hex)
        return;
    if (hex[0] == '0' && hex[1] == 'x')
        hex += 2;
    for (size_t i = 0; i < digits / 2 && hex[i * 2] && hex[i * 2 + 1]; i++) {
        unsigned v;
        if (sscanf(hex + i * 2, "%2x", &v) == 1)
            dst[i] = (uint8_t)v;
    }
}

size_t tb_detail_pack_dex_hop(uint8_t *out, size_t cap, const uint8_t *to20,
                              const char *calldata_hex, const char *result_hex)
{
    if (cap < TB_DEX_HOP_PACK_SIZE)
        return 0;
    memset(out, 0, TB_DEX_HOP_PACK_SIZE);
    if (to20)
        memcpy(out, to20, 20);
    hex_head(out + 20, calldata_hex, 16);
    hex_head(out + 36, result_hex, 16);
    return TB_DEX_HOP_PACK_SIZE;
}

size_t tb_detail_pack_curve(uint8_t *dst, size_t cap, const uint64_t size[3],
                            const uint64_t outs[3])
{
    if (cap < 48)
        return 0;
    memset(dst, 0, 48);
    for (int i = 0; i < 3; i++) {
        uint64_t s = size ? size[i] : 0;
        uint64_t o = outs ? outs[i] : 0;
        memcpy(dst + i * 16, &s, 8);
        memcpy(dst + i * 16 + 8, &o, 8);
    }
    return 48;
}

/* ---- экспандеры ---- */

static int bytes_hex(const uint8_t *p, size_t n, char *buf, size_t cap)
{
    size_t k = 0;
    for (size_t i = 0; i < n && k + 3 < cap; i++)
        k += (size_t)snprintf(buf + k, cap - k, "%02x", p[i]);
    return (int)k;
}

static int exp_net_raw(const tb_detail_t *d, char *buf, size_t cap)
{
    if (d->data_len < 4)
        return -1;
    int32_t status = (int32_t)((uint32_t)d->data[0]
                               | ((uint32_t)d->data[1] << 8)
                               | ((uint32_t)d->data[2] << 16)
                               | ((uint32_t)d->data[3] << 24));
    size_t body = d->data_len - 4;
    int n = snprintf(buf, cap, "HTTP %d, тело[%zu]='%.*s'", status, body,
                     (int)body, (const char *)d->data + 4);
    return n;
}

static int exp_dex_hop(const tb_detail_t *d, char *buf, size_t cap)
{
    if (d->data_len < TB_DEX_HOP_PACK_SIZE)
        return -1;
    char to[43], cd[36], rs[36];
    for (int i = 0; i < 20; i++)
        snprintf(to + i * 2, 3, "%02x", d->data[i]);
    bytes_hex(d->data + 20, 16, cd, sizeof(cd));
    bytes_hex(d->data + 36, 16, rs, sizeof(rs));
    return snprintf(buf, cap, "to=0x%s calldata=0x%s… result=0x%s…", to, cd,
                    rs);
}

static int exp_curve(const tb_detail_t *d, char *buf, size_t cap)
{
    if (d->data_len < 48)
        return -1;
    int n = 0;
    n += snprintf(buf + n, cap - (size_t)n, "точки size→out:");
    for (int i = 0; i < 3 && (size_t)n < cap; i++) {
        uint64_t s, o;
        memcpy(&s, d->data + i * 16, 8);
        memcpy(&o, d->data + i * 16 + 8, 8);
        n += snprintf(buf + n, cap - (size_t)n, " %llu→%llu",
                      (unsigned long long)s, (unsigned long long)o);
    }
    return n;
}

int tb_detail_expand(const tb_detail_t *d, char *buf, size_t cap)
{
    if (!d || !buf || cap == 0)
        return -1;
    if (d->group == TB_G_NET && d->cmd == TB_CMD_NET_RAW)
        return exp_net_raw(d, buf, cap);
    if (d->group == TB_G_DEX && d->cmd == TB_CMD_DEX_HOP)
        return exp_dex_hop(d, buf, cap);
    if (d->group == TB_G_DECISION && d->cmd == TB_CMD_DECIDE_CURVE)
        return exp_curve(d, buf, cap);
    return snprintf(buf, cap, "[%s/%u] %u байт", tb_group_name(d->group),
                    (unsigned)d->cmd, (unsigned)d->data_len);
}

size_t tb_error_format_details(const struct tb_error *err, char *buf, size_t cap)
{
    if (!err || !buf || cap == 0)
        return 0;
    size_t n = 0;
    int wrote = 0;
#define APPEND(...)                                                           \
    do {                                                                      \
        int rc_ = snprintf(buf + n, cap - n, __VA_ARGS__);                    \
        if (rc_ < 0)                                                          \
            return n;                                                         \
        n += ((size_t)rc_ < cap - n) ? (size_t)rc_ : (cap - n - 1);           \
    } while (0)

    for (int i = 0; i < TB_ERROR_DETAILS_MAX; i++) {
        const tb_detail_t *d = &err->details[i];
        if (d->data_len == 0 && d->cmd == 0)
            continue;
        char one[192];
        if (tb_detail_expand(d, one, sizeof(one)) < 0)
            continue;
        APPEND("%s[%s/%u] %s", wrote ? "; " : "", tb_group_name(d->group),
               (unsigned)d->cmd, one);
        wrote = 1;
    }
    if (wrote)
        return n;
    (void)wrote;
    buf[0] = '\0';
    return 0;
#undef APPEND
}
