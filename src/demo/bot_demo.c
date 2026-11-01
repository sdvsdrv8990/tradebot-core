#define TB_CURRENT_GROUP TB_G_DEMO /* зона ответственности файла */
/* ==========================================================================
 *  demo/bot_demo.c — демонстрация ядра v0.1 на симуляторе опроса источников.
 *
 *  Что показывает:
 *   - единый событийный цикл: таймеры, приоритеты, периодические задачи;
 *   - блокирующие "HTTP-запросы" в пуле потоков, продолжения — в цикле;
 *   - стандартизацию ответов: HTTP -> унифицированный код (реестр)
 *     -> API Response Envelope (JSON);
 *   - ветку SUCCESS / ERROR / PENDING;
 *   - 429 с Retry-After: задержка никогда не меньше разрешённой;
 *   - экспоненциальный backoff + jitter;
 *   - 401: перманентная ошибка — опрос останавливается (ожидание не лечит);
 *   - circuit breaker: CLOSED -> OPEN -> HALF_OPEN -> CLOSED;
 *   - token-bucket: переполнение лимита -> отложенный повтор;
 *   - Event Ledger: JSONL-журнал + дедупликация по uuid;
 *   - глобальный обработчик ошибок на всё приложение.
 *
 *  Все ответы поставщиков симулированы; числа иллюстративны.
 *
 *  Запуск: bot_demo [--duration МС] [--ledger ПУТЬ] [--seed N]
 * ========================================================================== */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tb/arch/arch.h"
#include "tb/core/error.h"
#include "tb/core/error_registry.h"
#include "tb/core/log.h"
#include "tb/core/time.h"
#include "tb/json/envelope.h"
#include "tb/ledger/ledger.h"
#include "tb/runtime/breaker.h"
#include "tb/runtime/ratelimit.h"
#include "tb/sched/backoff.h"
#include "tb/sched/loop.h"
#include "tb/sched/threadpool.h"

/* ---- Симулированный шаг ответа поставщика ---- */
typedef struct {
    int         http_status;
    uint32_t    retry_after_ms; /* как HTTP-заголовок Retry-After, но в мс */
    const char *body;           /* фрагмент тела для pattern-правил */
    uint64_t    latency_ms;     /* имитация сетевой задержки */
} demo_step_t;

typedef struct demo_app demo_app_t;

typedef struct demo_provider {
    const char   *name;
    const char   *operation;
    const char   *data_template;   /* SUCCESS-payload (демо-данные) */
    demo_app_t   *app;

    tb_ratelimit_t rl;
    tb_breaker_t   breaker;
    tb_backoff_t   backoff;

    const demo_step_t *script;
    size_t         script_len, script_pos;

    uint64_t poll_interval_ms;
    uint64_t pending_delay_ms;
    uint64_t request_seq;

    uint32_t attempt;
    bool     stopped;

    /* статистика */
    uint64_t ok, err, rate_limited, server_err, unauthorized;
    uint64_t pending, deferred, circuit_rejected;
} demo_provider_t;

struct demo_app {
    tb_loop_t   *loop;
    tb_ledger_t *ledger;
    uint64_t     duration_ms;
    uint64_t     started_ms;

    demo_provider_t providers[4];
    size_t          nproviders;

    uint64_t discovery_tick;
    uint64_t archive_flushes;
    uint64_t env_by_status[3];

    /* глобальный обработчик ошибок */
    uint64_t err_by_sev[4];
    uint64_t err_by_domain[8];
    uint64_t errors_reported;
    bool     in_error_handler;
};

static tb_loop_t *g_demo_loop; /* для SIGINT */

/* ======================================================================
 *  Вспомогательные
 * ====================================================================== */

static size_t json_escape(const char *in, char *out, size_t cap)
{
    size_t n = 0;
    if (!in) { if (cap) out[0] = '\0'; return 0; }
    for (const unsigned char *p = (const unsigned char *)in; *p && n + 8 < cap; p++) {
        if (*p == '"' || *p == '\\') { out[n++] = '\\'; out[n++] = (char)*p; }
        else if (*p == '\n') { out[n++] = '\\'; out[n++] = 'n'; }
        else if (*p == '\r') { out[n++] = '\\'; out[n++] = 'r'; }
        else if (*p == '\t') { out[n++] = '\\'; out[n++] = 't'; }
        else if (*p < 0x20)  { n += (size_t)snprintf(out + n, cap - n, "\\u%04x", *p); }
        else out[n++] = (char)*p;
    }
    out[n] = '\0';
    return n;
}

static void demo_emit_envelope(demo_app_t *app, const tb_envelope_t *env)
{
    char buf[2048];
    size_t len;
    tb_error_t err = TB_ERROR_INIT;
    if (tb_envelope_serialize(env, buf, sizeof(buf), &len, &err) != 0) {
        TB_LOG_ERROR("demo: сериализация конверта: %s", err.message);
        return;
    }
    if (env->status >= 0 && env->status <= 2)
        app->env_by_status[env->status]++;

    tb_error_t lerr = TB_ERROR_INIT;
    if (tb_ledger_emit(app->ledger, "api.response", buf, NULL, NULL, &lerr) != 0)
        TB_LOG_ERROR("demo: журнал отклонил конверт: %s", lerr.message);
}

/* ======================================================================
 *  Глобальный обработчик ошибок (на всё приложение)
 * ====================================================================== */

static void demo_error_handler(const tb_error_t *err, void *user)
{
    demo_app_t *app = user;
    if (!err || !app || app->in_error_handler) return;

    unsigned sev = err->severity < 4 ? err->severity : 3;
    unsigned dom = err->domain < 8 ? err->domain : 0;
    app->err_by_sev[sev]++;
    app->err_by_domain[dom]++;
    app->errors_reported++;

    if (sev >= TB_SEV_ERROR) {
        char line[512];
        tb_error_format(err, line, sizeof(line));
        TB_LOG_ERROR("app: %s", line);
    }

    /* В журнал — WARN и выше (INFO-события уже представлены конвертами). */
    if (sev >= TB_SEV_WARN && app->ledger) {
        app->in_error_handler = true;
        const tb_error_meta_t *meta = tb_registry_meta(err->domain, err->code);
        char msg_esc[2 * TB_ERROR_MSG_MAX];
        json_escape(err->message, msg_esc, sizeof(msg_esc));
        char payload[2 * TB_ERROR_MSG_MAX + 256];
        snprintf(payload, sizeof(payload),
                 "{\"domain\":\"%s\",\"code\":\"%s\",\"severity\":\"%s\","
                 "\"retryable\":%s,\"http_status\":%d,\"retry_after_ms\":%u,"
                 "\"message\":\"%s\"}",
                 tb_domain_name(err->domain),
                 meta ? meta->name : "UNKNOWN",
                 tb_severity_name((tb_severity_t)err->severity),
                 err->retryable ? "true" : "false",
                 err->http_status, err->retry_after_ms, msg_esc);
        tb_ledger_emit(app->ledger, "error.reported", payload, NULL, NULL, NULL);
        app->in_error_handler = false;
    }
}

/* ======================================================================
 *  Опрос источника: цикл -> пул -> цикл
 * ====================================================================== */

typedef struct {
    demo_app_t     *app;
    demo_provider_t *p;
    demo_step_t     step;
    uint64_t        request_id;
} demo_fetch_t;

/* Воркер пула: имитация блокирующего HTTP-вызова. */
static void demo_fetch_job(void *arg)
{
    demo_fetch_t *f = arg;
    tb_sleep_ms(f->step.latency_ms);
    /* Ответ уже зафиксирован на стороне цикла (script). */
}

static void demo_log_response(const demo_provider_t *p, const tb_envelope_t *env,
                              const tb_error_t *err, uint64_t delay_ms)
{
    if (env->status == TB_ENV_SUCCESS) {
        TB_LOG_INFO("%-13s %-19s -> SUCCESS", p->name, p->operation);
    } else if (env->status == TB_ENV_PENDING) {
        TB_LOG_INFO("%-13s %-19s -> PENDING (фоновая задача, повтор через %llu мс)",
                    p->name, p->operation, (unsigned long long)delay_ms);
    } else {
        const tb_error_meta_t *meta = tb_registry_meta(err->domain, err->code);
        TB_LOG_INFO("%-13s %-19s -> ERROR %s (HTTP %d, попытка %u, повтор через %llu мс)%s",
                    p->name, p->operation,
                    meta ? meta->name : "?", err->http_status,
                    env->attempt, (unsigned long long)delay_ms,
                    p->stopped ? " — ОСТАНОВКА" : "");
    }
}

/* Цикл: продолжение после "HTTP-ответа". */
static void demo_poll(tb_loop_t *loop, tb_task_t *task, void *arg);

static void demo_on_response(tb_loop_t *loop, void *arg)
{
    demo_fetch_t    *f = arg;
    demo_provider_t *p = f->p;
    demo_app_t      *app = f->app;

    tb_error_t err = TB_ERROR_INIT;
    tb_error_clear(&err);
    bool is_err = tb_registry_map_response(p->name, f->step.http_status,
                                           f->step.body,
                                           f->step.body ? strlen(f->step.body) : 0,
                                           f->step.retry_after_ms, &err);

    tb_envelope_t env;
    tb_envelope_init(&env, p->name, p->operation, f->request_id);

    uint64_t delay_ms = p->poll_interval_ms;

    if (!is_err) {
        tb_envelope_set_data(&env, p->data_template, NULL);
        p->ok++;
        p->attempt = 0;
        tb_breaker_success(&p->breaker);
    } else {
        TB_ERROR_WRAP(&err, "provider=%s op=%s", p->name, p->operation);
        tb_report(&err); /* глобальный обработчик: счётчики + журнал */
        tb_envelope_set_error(&env, &err);
        p->attempt++;

        if (err.code == TB_API_PENDING) {
            env.status = TB_ENV_PENDING;
            p->pending++;
            delay_ms = p->pending_delay_ms;
            tb_envelope_set_retry(&env, p->attempt,
                                  (int64_t)(tb_wall_ms() + delay_ms));
        } else {
            p->err++;
            if (err.code == TB_API_RATE_LIMIT)   p->rate_limited++;
            if (err.code == TB_API_SERVER)       p->server_err++;
            if (err.code == TB_API_UNAUTHORIZED) p->unauthorized++;

            if (!err.retryable) {
                /* Ошибка авторизации/запроса не исправляется ожиданием. */
                p->stopped = true;
                tb_envelope_set_retry(&env, p->attempt, -1);
                char payload[256];
                const tb_error_meta_t *meta = tb_registry_meta(err.domain, err.code);
                snprintf(payload, sizeof(payload),
                         "{\"source\":\"%s\",\"code\":\"%s\",\"reason\":\"permanent\"}",
                         p->name, meta ? meta->name : "UNKNOWN");
                tb_ledger_emit(app->ledger, "provider.stopped", payload,
                               NULL, NULL, NULL);
                TB_LOG_WARN("%s: опрос остановлен — %s повторами не исправляется",
                            p->name, meta ? meta->name : "ошибка");
            } else {
                /* Retry-After доминирует над jitter внутри backoff. */
                delay_ms = tb_backoff_delay(&p->backoff, p->attempt, &err);
                tb_breaker_failure(&p->breaker, tb_now_ms());
                tb_envelope_set_retry(&env, p->attempt,
                                      (int64_t)(tb_wall_ms() + delay_ms));
            }
        }
    }

    demo_log_response(p, &env, &err, delay_ms);
    demo_emit_envelope(app, &env);

    if (!p->stopped)
        tb_loop_call_later(loop, delay_ms, TB_PRIO_NORMAL, "poll",
                           demo_poll, p, NULL);
}

/* Цикл: начало попытки опроса. */
static void demo_poll(tb_loop_t *loop, tb_task_t *task, void *arg)
{
    demo_provider_t *p = arg;
    (void)task;

    if (p->stopped) return;
    uint64_t now = tb_now_ms();

    /* 1. Предохранитель источника. */
    if (!tb_breaker_allow(&p->breaker, now)) {
        uint64_t wait = tb_breaker_retry_in_ms(&p->breaker, now);
        if (wait == 0) wait = 50;
        else wait += 50;
        p->circuit_rejected++;

        tb_error_t err = TB_ERROR_INIT;
        TB_ERROR_SET(&err, TB_DOMAIN_API, TB_API_CIRCUIT_OPEN,
                     "%s: предохранитель разомкнут, проба через %llu мс",
                     p->name, (unsigned long long)wait);
        tb_report(&err);

        tb_envelope_t env;
        tb_envelope_init(&env, p->name, p->operation, ++p->request_seq);
        tb_envelope_set_error(&env, &err);
        tb_envelope_set_retry(&env, p->attempt, (int64_t)(tb_wall_ms() + wait));
        demo_emit_envelope(p->app, &env);
        TB_LOG_WARN("%-13s %-19s -> CIRCUIT_OPEN (повтор через %llu мс)",
                    p->name, p->operation, (unsigned long long)wait);

        tb_loop_call_later(loop, wait, TB_PRIO_NORMAL, "poll", demo_poll, p, NULL);
        return;
    }

    /* 2. Общий ограничитель запросов источника. */
    if (!tb_ratelimit_acquire(&p->rl, now)) {
        uint64_t wait = tb_ratelimit_next_slot_ms(&p->rl, now)
                        + (tb_rand_next(&p->backoff) % 100);
        p->deferred++;
        char payload[192];
        snprintf(payload, sizeof(payload),
                 "{\"source\":\"%s\",\"wait_ms\":%llu}",
                 p->name, (unsigned long long)wait);
        tb_ledger_emit(p->app->ledger, "ratelimit.deferred", payload,
                       NULL, NULL, NULL);
        TB_LOG_INFO("%-13s %-19s .. лимит исчерпан, повтор через %llu мс",
                    p->name, p->operation, (unsigned long long)wait);
        tb_loop_call_later(loop, wait, TB_PRIO_NORMAL, "poll", demo_poll, p, NULL);
        return;
    }

    /* 3. Блокирующий вызов — в пул; продолжение вернётся в цикл. */
    demo_fetch_t *f = calloc(1, sizeof(*f));
    if (!f) {
        TB_LOG_ERROR("demo: нет памяти на запрос %s", p->name);
        tb_loop_call_later(loop, 1000, TB_PRIO_NORMAL, "poll", demo_poll, p, NULL);
        return;
    }
    f->app = p->app;
    f->p   = p;
    f->step = p->script[p->script_pos % p->script_len];
    p->script_pos++;
    f->request_id = ++p->request_seq;

    tb_pool_submit_t s = {
        .job       = demo_fetch_job,
        .arg       = f,
        .arg_dtor  = free,
        .loop      = loop,
        .done_prio = TB_PRIO_HIGH,
        .done      = demo_on_response,
    };
    if (!tb_threadpool_submit(tb_loop_pool(loop), &s)) {
        free(f);
        TB_LOG_ERROR("demo: пул отклонил запрос %s", p->name);
        tb_loop_call_later(loop, 1000, TB_PRIO_NORMAL, "poll", demo_poll, p, NULL);
    }
}

/* ======================================================================
 *  Периодические демонстрационные задачи
 * ====================================================================== */

static void demo_discovery(tb_loop_t *loop, tb_task_t *task, void *arg)
{
    demo_app_t *app = arg;
    (void)loop; (void)task;

    static const char *tokens[]   = { "TOKEN_A", "TOKEN_B", "TOKEN_C", "TOKEN_D" };
    static const char *networks[] = { "Base", "Arbitrum", "BSC", "Avalanche" };
    uint64_t i = app->discovery_tick++;

    char payload[192], uuid[64];
    snprintf(payload, sizeof(payload),
             "{\"token\":\"%s\",\"network\":\"%s\",\"source\":\"market_scan\"}",
             tokens[i % 4], networks[(i / 4) % 4]);
    snprintf(uuid, sizeof(uuid), "cand-%llu", (unsigned long long)i);
    tb_ledger_emit(app->ledger, "discovery.candidate", payload, uuid, NULL, NULL);
}

static void demo_heartbeat(tb_loop_t *loop, tb_task_t *task, void *arg)
{
    demo_app_t *app = arg;
    (void)loop; (void)task;

    /* Фиксированный uuid: событие пишется один раз, далее — дедупликация. */
    tb_error_t err = TB_ERROR_INIT;
    tb_ledger_emit(app->ledger, "heartbeat",
                   "{\"status\":\"alive\"}", "demo:heartbeat", NULL, &err);
    if (tb_error_is_set(&err) && err.code == TB_LEDGER_DUP)
        TB_LOG_INFO("heartbeat: дубликат пропущен (uuid=demo:heartbeat), всего %llu",
                    (unsigned long long)tb_ledger_stats(app->ledger)->events_duplicate);
}

static void demo_stats_report(tb_loop_t *loop, tb_task_t *task, void *arg)
{
    demo_app_t *app = arg;
    (void)task;
    const tb_loop_stats_t *ls = tb_loop_stats(loop);
    const tb_ledger_stats_t *lds = tb_ledger_stats(app->ledger);

    char payload[1024];
    int n = snprintf(payload, sizeof(payload),
                     "{\"elapsed_ms\":%llu,\"tasks_run\":%llu,\"timers_fired\":%llu,"
                     "\"ledger_events\":%llu,\"duplicates\":%llu,\"errors_reported\":%llu,"
                     "\"providers\":{",
                     (unsigned long long)(tb_now_ms() - app->started_ms),
                     (unsigned long long)ls->tasks_run,
                     (unsigned long long)ls->timers_fired,
                     (unsigned long long)lds->events_written,
                     (unsigned long long)lds->events_duplicate,
                     (unsigned long long)app->errors_reported);
    for (size_t i = 0; i < app->nproviders && n < (int)sizeof(payload) - 160; i++) {
        const demo_provider_t *p = &app->providers[i];
        n += snprintf(payload + n, sizeof(payload) - (size_t)n,
                      "%s\"%s\":{\"ok\":%llu,\"err\":%llu,\"pend\":%llu,\"defer\":%llu,\"circ\":%llu}",
                      i ? "," : "", p->name,
                      (unsigned long long)p->ok, (unsigned long long)p->err,
                      (unsigned long long)p->pending, (unsigned long long)p->deferred,
                      (unsigned long long)p->circuit_rejected);
    }
    snprintf(payload + n, sizeof(payload) - (size_t)n, "}}");
    tb_ledger_emit(app->ledger, "demo.stats", payload, NULL, NULL, NULL);

    TB_LOG_INFO("статистика: задач=%llu таймеров=%llu конвертов(S/E/P)=%llu/%llu/%llu "
                "событий=%llu дубликатов=%llu",
                (unsigned long long)ls->tasks_run,
                (unsigned long long)ls->timers_fired,
                (unsigned long long)app->env_by_status[TB_ENV_SUCCESS],
                (unsigned long long)app->env_by_status[TB_ENV_ERROR],
                (unsigned long long)app->env_by_status[TB_ENV_PENDING],
                (unsigned long long)lds->events_written,
                (unsigned long long)lds->events_duplicate);
}

/* Воркер пула: имитация архивной записи (блокирующая). */
static void demo_archive_job(void *arg)
{
    demo_app_t *app = arg;
    tb_sleep_ms(120);
    app->archive_flushes++;
}

static void demo_archive_done(tb_loop_t *loop, void *arg)
{
    demo_app_t *app = arg;
    char payload[192];
    snprintf(payload, sizeof(payload),
             "{\"files\":%llu,\"latency_ms\":120,\"thread\":\"pool->loop\"}",
             (unsigned long long)app->archive_flushes);
    tb_ledger_emit(app->ledger, "archive.flushed", payload, NULL, NULL, NULL);
    (void)loop;
}

/* Цикл: раз в период отправляем архивную запись в пул. */
static void demo_archive_tick(tb_loop_t *loop, tb_task_t *task, void *arg)
{
    demo_app_t *app = arg;
    (void)task;
    tb_pool_submit_t s = {
        .job       = demo_archive_job,
        .arg       = app,
        .arg_dtor  = NULL,
        .loop      = loop,
        .done_prio = TB_PRIO_BACKGROUND,
        .done      = demo_archive_done,
    };
    if (!tb_threadpool_submit(tb_loop_pool(loop), &s))
        TB_LOG_WARN("demo: архив — пул закрыт, пропуск итерации");
}

static void demo_stop(tb_loop_t *loop, tb_task_t *task, void *arg)
{
    (void)task; (void)arg;
    TB_LOG_INFO("таймер длительности истёк — штатная остановка");
    tb_loop_stop(loop);
}

static void demo_sigint(int sig)
{
    (void)sig;
    /* tb_loop_stop: атомарный флаг + write(eventfd) — допустимо из сигнала. */
    if (g_demo_loop)
        tb_loop_stop(g_demo_loop);
}

/* ======================================================================
 *  main
 * ====================================================================== */

int main(int argc, char **argv)
{
    uint64_t duration_ms = 15000;
    const char *ledger_path = "tradebot_events.jsonl";
    uint64_t seed = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc)
            duration_ms = strtoull(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--ledger") == 0 && i + 1 < argc)
            ledger_path = argv[++i];
        else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc)
            seed = strtoull(argv[++i], NULL, 10);
        else {
            fprintf(stderr, "использование: %s [--duration МС] [--ledger ПУТЬ] [--seed N]\n",
                    argv[0]);
            return 2;
        }
    }

    tb_arch_init();
    if (seed == 0)
        seed = tb_rdtsc();
    tb_log_set_level(TB_LOG_INFO);

    char arch[128];
    printf("=================================================================\n");
    printf(" tradebot-core v0.1 — демо ядра (симуляция опроса источников)\n");
    printf(" архитектура: %s\n", tb_arch_desc(arch, sizeof(arch)));
    printf(" длительность: %llu мс | seed: %llu | журнал: %s\n",
           (unsigned long long)duration_ms, (unsigned long long)seed, ledger_path);
    printf("=================================================================\n");

    demo_app_t app;
    memset(&app, 0, sizeof(app));
    app.duration_ms = duration_ms;
    app.started_ms = tb_now_ms();

    tb_set_error_handler(demo_error_handler, &app);

    tb_error_t err = TB_ERROR_INIT;
    app.ledger = tb_ledger_open(ledger_path, TB_LEDGER_FLUSH_LINE, &err);
    if (!app.ledger) {
        char line[256];
        tb_error_format(&err, line, sizeof(line));
        fprintf(stderr, "не удалось открыть журнал: %s\n", line);
        return 1;
    }

    app.loop = tb_loop_create(2, &err); /* 2 воркера блокирующих операций */
    if (!app.loop) {
        char line[256];
        tb_error_format(&err, line, sizeof(line));
        fprintf(stderr, "не удалось создать цикл: %s\n", line);
        tb_ledger_close(app.ledger);
        return 1;
    }
    g_demo_loop = app.loop;
    signal(SIGINT, demo_sigint);
    signal(SIGTERM, demo_sigint);

    /* ---- Конфигурация источников (числа демонстрационные) ---- */

    static const demo_step_t gt_script[] = {
        { 200, 0,    NULL,          25 },
        { 429, 3000, "rate limit",  25 },
        { 429, 3000, "rate limit",  25 },
        { 429, 3000, "rate limit",  25 },
        { 200, 0,    NULL,          25 },
        { 200, 0,    NULL,          25 },
        { 429, 3000, "rate limit",  25 },
        { 200, 0,    NULL,          25 },
    };
    static const demo_step_t cg_script[] = {
        { 200, 0, NULL, 30 },
        { 202, 0, "queued", 30 },
        { 200, 0, NULL, 30 },
        { 200, 0, NULL, 30 },
        { 429, 1500, "rate limit", 30 },
        { 200, 0, NULL, 30 },
        { 200, 0, NULL, 30 },
    };
    static const demo_step_t gp_script[] = {
        { 200, 0, NULL, 20 },
        { 200, 0, NULL, 20 },
        { 401, 0, "unauthorized", 20 },
    };
    static const demo_step_t dl_script[] = {
        { 200, 0, NULL, 25 },
        { 503, 0, NULL, 25 },
        { 503, 0, NULL, 25 },
        { 503, 0, NULL, 25 },
        { 503, 0, NULL, 25 },
        { 200, 0, NULL, 25 },
        { 200, 0, NULL, 25 },
        { 503, 0, NULL, 25 },
        { 200, 0, NULL, 25 },
    };

    demo_provider_t providers[] = {
        {
            .name = "geckoterminal", .operation = "get_pool_ohlcv",
            .data_template =
                "{\"pool\":\"0x1a2b...c9d0\",\"price\":1.25,\"volume_24h\":125000,\"liquidity\":850000}",
            .script = gt_script, .script_len = 8,
            .poll_interval_ms = 800, .pending_delay_ms = 1200,
        },
        {
            .name = "coingecko", .operation = "historical_analysis",
            .data_template =
                "{\"coin\":\"eth\",\"granularity\":\"1h\",\"points\":720}",
            .script = cg_script, .script_len = 7,
            .poll_interval_ms = 500, .pending_delay_ms = 1200,
        },
        {
            .name = "goplus", .operation = "token_security",
            .data_template =
                "{\"token\":\"0xabc...def\",\"risk_score\":8,\"issues\":[]}",
            .script = gp_script, .script_len = 3,
            .poll_interval_ms = 1500, .pending_delay_ms = 1200,
        },
        {
            .name = "defillama", .operation = "chains_tvl",
            .data_template =
                "{\"chain\":\"base\",\"tvl\":1450000000,\"change_24h\":0.031}",
            .script = dl_script, .script_len = 9,
            .poll_interval_ms = 1000, .pending_delay_ms = 1200,
        },
    };

    app.nproviders = sizeof(providers) / sizeof(providers[0]);
    for (size_t i = 0; i < app.nproviders; i++) {
        demo_provider_t *p = &providers[i];
        p->app = &app;

        uint32_t rate = 30, burst = 30;
        if (strcmp(p->name, "coingecko") == 0) { rate = 12; burst = 4; }
        if (strcmp(p->name, "defillama") == 0) { rate = 10; burst = 10; }

        tb_ratelimit_init(&p->rl, rate, burst);
        tb_breaker_init(&p->breaker,
                        strcmp(p->name, "geckoterminal") == 0 ? 3 : 4,
                        strcmp(p->name, "geckoterminal") == 0 ? 4000 : 6000);
        tb_backoff_init(&p->backoff, 900, 8000, 0.30, seed + i * 7919);

        app.providers[i] = *p; /* копия в app: провайдеры живут в app */
    }

    /* Старт опроса со сдвигом (анти-синхронизация). */
    uint64_t starts[4] = { 150, 250, 400, 300 };
    for (size_t i = 0; i < app.nproviders; i++) {
        demo_provider_t *p = &app.providers[i];
        tb_loop_call_later(app.loop, starts[i], TB_PRIO_NORMAL, "poll",
                           demo_poll, p, NULL);
    }

    /* Периодические демонстрационные задачи. */
    tb_loop_call_every(app.loop, 900, TB_PRIO_LOW, "discovery",
                       demo_discovery, &app, NULL);
    tb_loop_call_every(app.loop, 1800, TB_PRIO_NORMAL, "heartbeat",
                       demo_heartbeat, &app, NULL);
    tb_loop_call_every(app.loop, 3000, TB_PRIO_NORMAL, "stats",
                       demo_stats_report, &app, NULL);

    /* Архивная запись: каждые 4 с блокирующая работа в пуле. */
    tb_loop_call_every(app.loop, 4000, TB_PRIO_NORMAL, "archive",
                       demo_archive_tick, &app, NULL);

    /* Остановка по таймеру длительности. */
    tb_loop_call_later(app.loop, duration_ms, TB_PRIO_CRITICAL, "stop",
                       demo_stop, &app, NULL);

    /* ---- Запуск цикла в главном потоке ---- */
    int rc = tb_loop_run(app.loop);

    /* ---- Итоги ---- */
    const tb_loop_stats_t *ls = tb_loop_stats(app.loop);
    const tb_ledger_stats_t *lds = tb_ledger_stats(app.ledger);
    uint64_t elapsed = tb_now_ms() - app.started_ms;

    printf("\n======================= ИТОГИ ДЕМО =======================\n");
    printf("Время работы            : %llu мс\n", (unsigned long long)elapsed);
    printf("Цикл: задач исполнено   : %llu (постингов %llu, отмен %llu)\n",
           (unsigned long long)ls->tasks_run,
           (unsigned long long)ls->tasks_posted,
           (unsigned long long)ls->tasks_canceled);
    printf("Цикл: таймеров сработало: %llu (каскадов %llu, пробуждений %llu)\n",
           (unsigned long long)ls->timers_fired,
           (unsigned long long)ls->wheel_cascades,
           (unsigned long long)ls->wakeups);
    printf("Пул: завершённых работ  : %llu\n",
           (unsigned long long)tb_threadpool_completed(tb_loop_pool(app.loop)));
    printf("Конверты: SUCCESS/ERROR/PENDING : %llu / %llu / %llu\n",
           (unsigned long long)app.env_by_status[TB_ENV_SUCCESS],
           (unsigned long long)app.env_by_status[TB_ENV_ERROR],
           (unsigned long long)app.env_by_status[TB_ENV_PENDING]);
    printf("Ошибок сообщено глобальному обработчику: %llu "
           "(INFO %llu, WARN %llu, ERROR %llu)\n",
           (unsigned long long)app.errors_reported,
           (unsigned long long)app.err_by_sev[TB_SEV_INFO],
           (unsigned long long)app.err_by_sev[TB_SEV_WARN],
           (unsigned long long)app.err_by_sev[TB_SEV_ERROR]);
    printf("Домены ошибок: CORE=%llu SCHED=%llu API=%llu LEDGER=%llu JSON=%llu\n",
           (unsigned long long)app.err_by_domain[TB_DOMAIN_CORE],
           (unsigned long long)app.err_by_domain[TB_DOMAIN_SCHED],
           (unsigned long long)app.err_by_domain[TB_DOMAIN_API],
           (unsigned long long)app.err_by_domain[TB_DOMAIN_LEDGER],
           (unsigned long long)app.err_by_domain[TB_DOMAIN_JSON]);
    printf("Журнал: событий %llu, дубликатов пропущено %llu, байт %llu\n",
           (unsigned long long)lds->events_written,
           (unsigned long long)lds->events_duplicate,
           (unsigned long long)lds->bytes_written);
    printf("Журнал: %s\n", ledger_path);
    printf("\n %-13s %4s %4s %4s %4s %5s %6s %6s %s\n",
           "источник", "OK", "ERR", "429", "5xx", "PEND", "DEFER", "CIRC", "статус");
    for (size_t i = 0; i < app.nproviders; i++) {
        const demo_provider_t *p = &app.providers[i];
        printf(" %-13s %4llu %4llu %4llu %4llu %5llu %6llu %6llu %s\n",
               p->name,
               (unsigned long long)p->ok, (unsigned long long)p->err,
               (unsigned long long)p->rate_limited,
               (unsigned long long)p->server_err,
               (unsigned long long)p->pending,
               (unsigned long long)p->deferred,
               (unsigned long long)p->circuit_rejected,
               p->stopped ? "остановлен" : "активен");
    }
    printf("===========================================================\n");

    tb_loop_destroy(app.loop);
    tb_ledger_close(app.ledger);
    return rc == 0 ? 0 : 1;
}
