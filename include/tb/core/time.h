/* ==========================================================================
 *  tb/core/time.h — монотонное и настенное время, форматирование, сон.
 *  Все внутренние дедлайны планировщика — монотонные миллисекунды.
 * ========================================================================== */
#ifndef TB_CORE_TIME_H
#define TB_CORE_TIME_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint64_t tb_now_ms(void);   /* CLOCK_MONOTONIC, мс */
uint64_t tb_now_us(void);   /* CLOCK_MONOTONIC, мкс */
uint64_t tb_wall_ms(void);  /* epoch, мс */

/* ISO 8601 UTC: "2026-11-01T12:00:00.123Z" (buf >= 25 байт). */
void tb_format_iso8601(uint64_t wall_ms, char *buf, size_t cap);

void tb_sleep_ms(uint64_t ms);

#ifdef __cplusplus
}
#endif
#endif /* TB_CORE_TIME_H */
