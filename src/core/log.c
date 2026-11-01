#define TB_CURRENT_GROUP TB_G_CORE /* зона ответственности файла */
/* ==========================================================================
 *  tb/core/log.c — реализация логгера.
 * ========================================================================== */
#include "tb/core/log.h"
#include "tb/core/time.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static tb_log_level_t  g_level = TB_LOG_INFO;
static tb_log_sink_fn  g_sink  = NULL; /* NULL -> stderr */
static void           *g_sink_user;
static pthread_mutex_t g_lock  = PTHREAD_MUTEX_INITIALIZER;

const char *tb_log_level_name(tb_log_level_t lvl)
{
    switch (lvl) {
    case TB_LOG_TRACE: return "TRACE";
    case TB_LOG_DEBUG: return "DEBUG";
    case TB_LOG_INFO:  return "INFO";
    case TB_LOG_WARN:  return "WARN";
    case TB_LOG_ERROR: return "ERROR";
    case TB_LOG_FATAL: return "FATAL";
    default:           return "?";
    }
}

void tb_log_set_level(tb_log_level_t lvl) { g_level = lvl; }
tb_log_level_t tb_log_get_level(void)     { return g_level; }

void tb_log_set_sink(tb_log_sink_fn sink, void *user)
{
    pthread_mutex_lock(&g_lock);
    g_sink      = sink;
    g_sink_user = user;
    pthread_mutex_unlock(&g_lock);
}

static void default_sink(void *user, tb_log_level_t lvl,
                         const char *file, int line, const char *msg)
{
    (void)user;
    char ts[32];
    tb_format_iso8601(tb_wall_ms(), ts, sizeof(ts));
    const char *base = strrchr(file, '/');
    base = base ? base + 1 : file;
    fprintf(stderr, "%s %-5s %s:%d: %s\n", ts, tb_log_level_name(lvl), base, line, msg);
}

void tb_log_write(tb_log_level_t lvl, const char *file, int line,
                  const char *fmt, ...)
{
    if ((int)lvl < (int)g_level) return;

    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    pthread_mutex_lock(&g_lock);
    tb_log_sink_fn sink = g_sink;
    void *user = g_sink_user;
    pthread_mutex_unlock(&g_lock);

    if (sink) sink(user, lvl, file, line, msg);
    else      default_sink(NULL, lvl, file, line, msg);
}
