/* ==========================================================================
 *  tb/core/log.h — потокобезопасный логгер с уровнями и подключаемым sink.
 * ========================================================================== */
#ifndef TB_CORE_LOG_H
#define TB_CORE_LOG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TB_LOG_TRACE = 0,
    TB_LOG_DEBUG = 1,
    TB_LOG_INFO  = 2,
    TB_LOG_WARN  = 3,
    TB_LOG_ERROR = 4,
    TB_LOG_FATAL = 5
} tb_log_level_t;

const char *tb_log_level_name(tb_log_level_t lvl);

/* Приёмник логов. По умолчанию — stderr. user — произвольный контекст. */
typedef void (*tb_log_sink_fn)(void *user, tb_log_level_t lvl,
                               const char *file, int line, const char *msg);

void          tb_log_set_level(tb_log_level_t lvl);
tb_log_level_t tb_log_get_level(void);
void          tb_log_set_sink(tb_log_sink_fn sink, void *user);

void tb_log_write(tb_log_level_t lvl, const char *file, int line,
                  const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

#define TB_LOG(lvl, ...) tb_log_write((lvl), __FILE__, __LINE__, __VA_ARGS__)
#define TB_LOG_TRACE(...) TB_LOG(TB_LOG_TRACE, __VA_ARGS__)
#define TB_LOG_DEBUG(...) TB_LOG(TB_LOG_DEBUG, __VA_ARGS__)
#define TB_LOG_INFO(...)  TB_LOG(TB_LOG_INFO,  __VA_ARGS__)
#define TB_LOG_WARN(...)  TB_LOG(TB_LOG_WARN,  __VA_ARGS__)
#define TB_LOG_ERROR(...) TB_LOG(TB_LOG_ERROR, __VA_ARGS__)
#define TB_LOG_FATAL(...) TB_LOG(TB_LOG_FATAL, __VA_ARGS__)

#ifdef __cplusplus
}
#endif
#endif /* TB_CORE_LOG_H */
