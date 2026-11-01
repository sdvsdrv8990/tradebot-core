/* ==========================================================================
 *  tb/core/strutil.h — точечные строковые утилиты ядра (header-only).
 *
 *  tb_utf8_fit: откат УСЕЧЁННОЙ строки к последнему полному символу UTF-8.
 *  snprintf/vsnprintf режут по байтам и могут оборвать многобайтовую
 *  последовательность ровно на границе буфера — такие строки ломают
 *  строгую валидацию JSONL-журнала и отображение. Строки, не упиравшиеся
 *  в границу, не трогаются.
 * ========================================================================== */
#ifndef TB_CORE_STRUTIL_H
#define TB_CORE_STRUTIL_H

#include <stddef.h>
#include <string.h>

static inline void tb_utf8_fit(char *buf, size_t cap)
{
    size_t n = strlen(buf);
    if (n + 1 < cap)
        return; /* не упирался в границу — усечения не было */

    /* Сколько байтов-продолжений (10xxxxxx) в самом хвосте. */
    size_t back = 0;
    while (back < 4 && back < n &&
           ((unsigned char)buf[n - 1 - back] & 0xC0) == 0x80)
        back++;
    if (back >= n) {
        buf[0] = '\0'; /* сплошные продолжения — мусор */
        return;
    }

    unsigned char lead = (unsigned char)buf[n - 1 - back];
    size_t seqlen = (lead & 0xE0) == 0xC0 ? 2
                  : (lead & 0xF0) == 0xE0 ? 3
                  : (lead & 0xF8) == 0xF0 ? 4 : 1;

    /* Многобайтовый символ полон, только если за лидом идёт ровно
     * seqlen-1 продолжений; иначе отрезаем его целиком (вместе с лидом). */
    if ((lead & 0x80) != 0 && back + 1 != seqlen)
        n = n - 1 - back;
    buf[n] = '\0';
}

#endif /* TB_CORE_STRUTIL_H */
