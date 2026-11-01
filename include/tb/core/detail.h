/* ==========================================================================
 *  tb/core/detail.h — капсулы деталей: ленивое «подробнее» для ошибок.
 *
 *  Ошибка несёт компактные капсулы (сырые байты, усечённые до лимита):
 *  команда + группа + данные. Форматирование — только по требованию
 *  (tb_error_format_details / журнал для ERROR-severity): контекст не
 *  глотается и не раздувается — «нажать подробнее».
 *
 *  Каждая команда принадлежит группе (ids.h); встроенные экспандеры
 *  живут в реестре (detail.c) — один источник истины.
 * ==========================================================================
 */
#ifndef TB_CORE_DETAIL_H
#define TB_CORE_DETAIL_H

#include <stddef.h>
#include <stdint.h>
#include "tb/core/ids.h"

#ifdef __cplusplus
extern "C" {
#endif

struct tb_error; /* определён в tb/core/error.h (включает этот заголовок) */

#define TB_DETAIL_DATA_MAX    64
#define TB_ERROR_DETAILS_MAX  2

typedef struct tb_detail {
    uint16_t group;                          /* владелец команды */
    uint16_t cmd;                            /* tb_cmd_id_t */
    uint8_t  data[TB_DETAIL_DATA_MAX];       /* усечённые сырые данные */
    uint8_t  data_len;
} tb_detail_t;

/* Строковое представление капсулы (экспандер команды). Возврат — длина
 * записанного (без '\0'), <0 — команда неизвестна. */
int tb_detail_expand(const tb_detail_t *d, char *buf, size_t cap);

/* Развернуть все капсулы ошибки одной строкой:
 * "детали: [net/raw] ...; [dex/hop] ...". 0 — капсул нет. */
size_t tb_error_format_details(const struct tb_error *err, char *buf,
                                size_t cap);

/* Упаковка данных для встроенных команд (продюсеры). */
#define TB_NET_RAW_PACK_SIZE 64  /* [0..3] статус LE, [4..] тело */
#define TB_DEX_HOP_PACK_SIZE 52  /* [0..19] to, [20..35] calldata, [36..51] result */
#define TB_DECIDE_CURVE_POINTS 3

size_t tb_detail_pack_net_raw(uint8_t *out, size_t cap, int32_t status,
                              const char *body, size_t body_len);
/* to/calldata/result — байты; пакуются первые 16 байт calldata/result. */
size_t tb_detail_pack_dex_hop(uint8_t *out, size_t cap, const uint8_t *to20,
                            const char *calldata_hex, const char *result_hex);
/* Три точки (size, out) кривой profit(size). */
size_t tb_detail_pack_curve(uint8_t *dst, size_t cap, const uint64_t size[3],
                            const uint64_t outs[3]);

#ifdef __cplusplus
}
#endif
#endif /* TB_CORE_DETAIL_H */
