#define TB_CURRENT_GROUP TB_G_CORE /* зона ответственности файла */
/* ==========================================================================
 *  tb/core/time.c — реализация часов.
 * ========================================================================== */
#include "tb/core/time.h"

#include <errno.h>
#include <stdio.h>
#include <time.h>

uint64_t tb_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

uint64_t tb_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

uint64_t tb_wall_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

void tb_format_iso8601(uint64_t wall_ms, char *buf, size_t cap)
{
    if (!buf || cap < 25) return;
    time_t      sec  = (time_t)(wall_ms / 1000);
    unsigned    msec = (unsigned)(wall_ms % 1000);
    struct tm   tm_utc;
    gmtime_r(&sec, &tm_utc);
    snprintf(buf, cap, "%04d-%02d-%02dT%02d:%02d:%02d.%03uZ",
             tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
             tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec, msec);
}

void tb_sleep_ms(uint64_t ms)
{
    struct timespec ts = {
        .tv_sec  = (time_t)(ms / 1000ull),
        .tv_nsec = (long)((ms % 1000ull) * 1000000ull),
    };
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
        ; /* прерванный сон повторяем с остатком */
}
