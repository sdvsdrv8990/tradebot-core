#define TB_CURRENT_GROUP TB_G_DEMO /* зона ответственности файла */
/* ==========================================================================
 *  demo/events_demo.c — модуль регистрации событий в живом конвейере.
 *
 *  Что показывает (поверх ядра v0.1):
 *   - цепочка шагов собирается из модулей: core -> ratelimit -> breaker ->
 *     http -> http_resp; добавление/выгрузка шага НА ХОДУ — без
 *     переписывания цепочки (фазы ниже);
 *   - слежение за активными событиями: dump печатает QUEUED/ACTIVE/BLOCKED
 *     с полным текстовым следом «что принёс каждый шаг»;
 *   - контекст блокировок: rate limit (токен через N мс), breaker
 *     (opened_at / cooldown / probe_at — урок t=102 виден в следе),
 *     429 Retry-After, 503 backoff;
 *   - мост к планировщику: BLOCKED с wake_at -> tb_loop_call_later ->
 *     продолжение/повтор события по таймеру (наблюдатель состояний);
 *   - мост к Event Ledger: терминальные события уходят в JSONL-журнал,
 *     live-таблица хранит только живые;
 *   - printf-маркеры диагностики (этот файл собирается с TB_EVENT_MARKERS=1;
 *     вывод — в stderr, отделяйте от stdout: `./events_demo 2>markers.log`).
 *
 *  Фазы (мс от старта):
 *    5000  +cache  (шаг перед http: попадание отдаётся из кэша, без сети)
 *   13000  -cache
 *   16000  -ratelimit  (deferrals прекращаются)
 *   20000  -http       (в полёте — события перестраиваются с начала цепочки)
 *   23000  +http       (цепочка восстановлена на своё место)
 *   26000  стоп генератора, финальный dump
 *
 *  Запуск: events_demo [--duration МС] [--ledger ПУТЬ] [--seed N]
 * ========================================================================== */
#define TB_EVENT_MARKERS 1

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tb/arch/arch.h"
#include "tb/core/error.h"
#include "tb/core/error_registry.h"
#include "tb/core/log.h"
#include "tb/core/marker.h"
#include "tb/core/time.h"
#include "tb/events/builtins.h"
#include "tb/events/events.h"
#include "tb/ledger/ledger.h"
#include "tb/sched/backoff.h"
#include "tb/sched/loop.h"

/* ---- Пользовательские флаги события (биты TB_EV_FLAG_USER_MASK) ---- */
#define F_GOT200 0x20u /* шаг http_resp получил успешный ответ */

/* ---- Виды событий (kind) ---- */
enum { KIND_TVL = 0, KIND_YIELD = 1, KIND_COUNT = 2 };

static const char *kind_name(uint16_t kind)
{
    return kind == KIND_TVL ? "TVL_PULL" : "YIELD_SCAN";
}

/* ---- Симулированные ответы источника (DefiLlama-подобный сценарий) ---- */
typedef struct {
    int      status;
    uint32_t retry_after_ms;
    const char *note;
    uint64_t latency_ms;
} http_step_t;

static const http_step_t script[] = {
    { 200,    0, "tvl=1.45B",  30 },
    { 200,    0, "yield=4.1%", 30 },
    { 429, 1500, "rate limit", 25 },
    { 503,    0, "maintenance", 40 },
    { 503,    0, "maintenance", 40 },
    { 503,    0, "maintenance", 40 }, /* третий 503 подряд: breaker -> OPEN */
    { 200,    0, "tvl=1.47B",  30 }, /* проба HALF_OPEN закрывает */
    { 202,    0, "queued",     25 },
    { 200,    0, "yield=4.2%", 30 },
    { 200,    0, "tvl=1.48B",  30 },
    { 401,    0, "unauthorized", 20 }, /* перманентная: событие FAILED */
};
#define SCRIPT_LEN (sizeof(script) / sizeof(script[0]))

/* ---- Кэш ответов (модуль-шаг + наблюдатель-хранитель) ---- */
typedef struct {
    uint64_t stored_ms[KIND_COUNT];
    uint64_t ttl_ms;
    uint64_t hits, stores;
} demo_cache_t;

/* ---- Приложение ---- */
typedef struct demo_app {
    tb_evreg_t   *reg;
    tb_loop_t    *loop;
    tb_ledger_t  *ledger;
    const char   *ledger_path;

    tb_ratelimit_t rl;      /* 10 rpm — консервативно, как договорились */
    tb_breaker_t   breaker;
    tb_backoff_t   backoff;
    demo_cache_t   cache;

    size_t   script_pos;
    uint64_t gen_tick;
    uint64_t gen_task_id;
    bool     generating;

    /* счётчики для итогов */
    uint64_t http_sent, resp_ok, resp_pending, resp_429, resp_5xx;
} demo_app_t;

static tb_loop_t *g_loop; /* для SIGINT */

/* ======================================================================
 *  Шаг 1: core — вход в цепочку (контекст прохода)
 * ====================================================================== */
static tb_ev_action_t core_fn(tb_evreg_t *r, tb_event_t *ev, void *user)
{
    (void)user;
    tb_event_step(r, ev, "проход %u: kind=%s, цепочка из %zu шагов",
                  ev->attempts, kind_name(ev->kind),
                  tb_evreg_module_count(r));
    return TB_EV_ACT_CHAIN;
}

/* ======================================================================
 *  Шаги http: отправка и приём ответа (имитация полёта блокировкой)
 * ====================================================================== */
static tb_ev_action_t http_fn(tb_evreg_t *r, tb_event_t *ev, void *user)
{
    demo_app_t *d = user;
    uint64_t now = tb_now_ms();

    ev->tag = d->script_pos++; /* какой по счёту ответ придёт */
    const http_step_t *s = &script[ev->tag % SCRIPT_LEN];
    d->http_sent++;

    tb_event_step(r, ev, "GET %s отправлен (полёт %llu мс, отправка №%llu)",
                  kind_name(ev->kind), (unsigned long long)s->latency_ms,
                  (unsigned long long)(ev->tag + 1));
    tb_event_block_until(ev, now + s->latency_ms);
    return TB_EV_ACT_BLOCK; /* ждём «ответа»: продолжение — шаг http_resp */
}

static tb_ev_action_t http_resp_fn(tb_evreg_t *r, tb_event_t *ev, void *user)
{
    demo_app_t *d = user;
    uint64_t now = tb_now_ms();
    const http_step_t *s = &script[ev->tag % SCRIPT_LEN];

    tb_error_t err = TB_ERROR_INIT;
    switch (s->status) {
    case 200:
        tb_breaker_success(&d->breaker);
        d->resp_ok++;
        ev->flags |= F_GOT200;
        tb_event_step(r, ev, "HTTP 200: %s", s->note);
        return TB_EV_ACT_CHAIN; /* дальше — конец цепочки = DONE */

    case 202:
        tb_breaker_success(&d->breaker);
        d->resp_pending++;
        ev->flags |= F_GOT200;
        tb_event_step(r, ev, "HTTP 202: фоновая задача поставщика (%s)", s->note);
        return TB_EV_ACT_CHAIN;

    case 429:
        d->resp_429++;
        tb_registry_map_response("defillama", 429, NULL, 0, s->retry_after_ms, &err);
        tb_event_attach(ev, &err);
        tb_event_step(r, ev, "HTTP 429: Retry-After %u мс — повтор запроса",
                      s->retry_after_ms);
        tb_event_retry_at(ev, now + s->retry_after_ms); /* полный повтор */
        return TB_EV_ACT_BLOCK;

    case 503:
        d->resp_5xx++;
        tb_registry_map_response("defillama", 503, NULL, 0, 0, &err);
        tb_event_attach(ev, &err);
        tb_breaker_failure(&d->breaker, now);
        {
            uint64_t delay = tb_backoff_delay(&d->backoff, ev->attempts, &err);
            tb_event_step(r, ev,
                          "HTTP 503: сбой источника, backoff %llu мс, "
                          "сбоев подряд %u/%u",
                          (unsigned long long)delay,
                          d->breaker.consecutive_failures,
                          d->breaker.failure_threshold);
            tb_event_retry_at(ev, now + delay);
        }
        return TB_EV_ACT_BLOCK;

    default:
        tb_registry_map_response("defillama", s->status, NULL, 0, 0, &err);
        tb_event_attach(ev, &err);
        tb_event_step(r, ev, "HTTP %d: перманентная ошибка — событие провалено",
                      s->status);
        return TB_EV_ACT_FAIL;
    }
}

/* ======================================================================
 *  Шаг-кэш: HIT -> DONE без похода в сеть
 * ====================================================================== */
static tb_ev_action_t cache_fn(tb_evreg_t *r, tb_event_t *ev, void *user)
{
    demo_cache_t *c = user;
    uint64_t now = tb_now_ms();

    if (c->stored_ms[ev->kind] != 0 &&
        now - c->stored_ms[ev->kind] < c->ttl_ms) {
        c->hits++;
        tb_event_step(r, ev, "HIT: kind=%s из кэша (возраст %llu мс < ttl %llu)",
                      kind_name(ev->kind),
                      (unsigned long long)(now - c->stored_ms[ev->kind]),
                      (unsigned long long)c->ttl_ms);
        return TB_EV_ACT_DONE; /* сеть не трогаем */
    }
    tb_event_step(r, ev, "MISS: kind=%s", kind_name(ev->kind));
    return TB_EV_ACT_CHAIN;
}

/* Наблюдатель-хранитель: успешные ответы складываются в кэш. */
static void cache_store_observer(tb_evreg_t *r, tb_event_t *ev, void *user)
{
    demo_cache_t *c = user;
    (void)r;
    if (ev->state == TB_EV_DONE && (ev->flags & F_GOT200)) {
        c->stored_ms[ev->kind] = tb_now_ms();
        c->stores++;
    }
}

/* ======================================================================
 *  Мост к планировщику: BLOCKED(wake_at) -> таймер -> продолжение
 * ====================================================================== */
typedef struct {
    demo_app_t *app;
    uint64_t    event_id;
} resume_arg_t;

static void resume_task(tb_loop_t *loop, tb_task_t *task, void *arg)
{
    resume_arg_t *ra = arg;
    (void)loop;
    (void)task;

    tb_error_t err = TB_ERROR_INIT;
    tb_event_t *ev = tb_evreg_find(ra->app->reg, ra->event_id);
    if (!ev)
        return; /* уже закрыт (отменён/вытеснен) */
    if (ev->state != TB_EV_BLOCKED)
        return; /* уже продолжен кем-то */
    TB_MARK("demo: таймер будит событие #%llu", (unsigned long long)ev->id);
    if (tb_evreg_continue(ra->app->reg, ev, &err) != 0)
        TB_LOG_WARN("demo: продолжение #%llu не удалось: %s",
                    (unsigned long long)ev->id, err.message);
}

static void bridge_observer(tb_evreg_t *r, tb_event_t *ev, void *user)
{
    demo_app_t *d = user;
    (void)r;
    if (ev->state != TB_EV_BLOCKED || ev->wake_at_ms == 0)
        return; /* wake=0 — ждём внешнего продолжения, таймер не нужен */

    uint64_t now = tb_now_ms();
    uint64_t delay = ev->wake_at_ms > now ? ev->wake_at_ms - now : 0;

    resume_arg_t *ra = malloc(sizeof(*ra));
    if (!ra)
        return;
    ra->app = d;
    ra->event_id = ev->id;
    tb_loop_call_later(d->loop, delay, TB_PRIO_HIGH, "ev.resume",
                       resume_task, ra, free);
}

/* ======================================================================
 *  Генератор событий и фазы демо
 * ====================================================================== */
static void gen_task(tb_loop_t *loop, tb_task_t *task, void *arg)
{
    demo_app_t *d = arg;
    (void)loop;
    (void)task;
    if (!d->generating)
        return;

    uint16_t kind = (uint16_t)(d->gen_tick++ % KIND_COUNT);
    tb_error_t err = TB_ERROR_INIT;
    tb_event_t *ev = tb_event_create(d->reg, kind, 0, &err);
    if (!ev) {
        TB_LOG_WARN("demo: событие не создано: %s", err.message);
        return;
    }
    ev->tag = d->gen_tick;
    if (tb_evreg_dispatch(d->reg, ev, &err) != 0)
        TB_LOG_WARN("demo: прогон не запущен: %s", err.message);
}

static void load_cache(demo_app_t *d)
{
    tb_error_t err = TB_ERROR_INIT;
    if (tb_evreg_add_before(d->reg, "http", "cache", cache_fn, &d->cache,
                            &err) < 0) {
        TB_LOG_WARN("demo: cache не загружен: %s", err.message);
        return;
    }
    tb_evreg_on_state(d->reg, TB_EV_TERMINAL_MASK, cache_store_observer,
                      &d->cache);
    TB_LOG_INFO("фаза: шаг 'cache' ЗАГРУЖЕН перед 'http' — попадания не ходят в сеть");
}

static void unload_cache(demo_app_t *d)
{
    tb_evreg_remove(d->reg, "cache", NULL);
    tb_evreg_off_state(d->reg, cache_store_observer, &d->cache);
    TB_LOG_INFO("фаза: шаг 'cache' ВЫГРУЖЕН (hits=%llu, stores=%llu)",
                (unsigned long long)d->cache.hits,
                (unsigned long long)d->cache.stores);
}

typedef struct {
    demo_app_t *app;
    int         phase;
} phase_arg_t;

static void phase_task(tb_loop_t *loop, tb_task_t *task, void *arg);

static void schedule_phase(demo_app_t *d, int phase, uint64_t at_ms)
{
    phase_arg_t *pa = malloc(sizeof(*pa));
    if (!pa)
        return;
    pa->app = d;
    pa->phase = phase;
    tb_loop_call_at(d->loop, at_ms, TB_PRIO_NORMAL, "phase",
                    phase_task, pa, free);
}

static void phase_task(tb_loop_t *loop, tb_task_t *task, void *arg)
{
    phase_arg_t *pa = arg;
    demo_app_t *d = pa->app;
    (void)loop;
    (void)task;

    switch (pa->phase) {
    case 0:
        d->generating = true;
        TB_LOG_INFO("фаза: генератор событий запущен (900 мс, kinds чередуются)");
        d->gen_task_id = tb_loop_call_every(loop, 900, TB_PRIO_NORMAL,
                                            "gen", gen_task, d, NULL);
        break;
    case 1:
        load_cache(d);
        break;
    case 2:
        unload_cache(d);
        break;
    case 3:
        tb_evreg_remove(d->reg, "ratelimit", NULL);
        TB_LOG_INFO("фаза: шаг 'ratelimit' ВЫГРУЖЕН — отложенные проходы прекращаются");
        break;
    case 4:
        tb_evreg_remove(d->reg, "http", NULL);
        TB_LOG_INFO("фаза: шаг 'http' ВЫГРУЖЕН — события в полёте перестроятся "
                    "с начала цепочки, новые проходят до конца без сети");
        break;
    case 5:
        tb_evreg_add_before(d->reg, "http_resp", "http", http_fn, d, NULL);
        TB_LOG_INFO("фаза: шаг 'http' ВОЗВРАЩЁН на своё место (перед http_resp)");
        break;
    case 6:
        d->generating = false;
        if (d->gen_task_id)
            tb_loop_cancel(loop, d->gen_task_id);
        TB_LOG_INFO("фаза: генератор остановлен — финальное состояние реестра");
        printf("\n");
        tb_evreg_dump(d->reg, stdout);
        break;
    default:
        break;
    }
}

static void dump_task(tb_loop_t *loop, tb_task_t *task, void *arg)
{
    demo_app_t *d = arg;
    (void)loop;
    (void)task;
    printf("\n");
    tb_evreg_dump(d->reg, stdout);
}

static void demo_stop(tb_loop_t *loop, tb_task_t *task, void *arg)
{
    (void)task;
    (void)arg;
    TB_LOG_INFO("таймер длительности истёк — штатная остановка");
    tb_loop_stop(loop);
}

static void demo_sigint(int sig)
{
    (void)sig;
    if (g_loop)
        tb_loop_stop(g_loop);
}

/* ======================================================================
 *  main
 * ====================================================================== */
int main(int argc, char **argv)
{
    uint64_t duration_ms = 30000;
    const char *ledger_path = "events_demo.jsonl";
    uint64_t seed = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc)
            duration_ms = strtoull(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--ledger") == 0 && i + 1 < argc)
            ledger_path = argv[++i];
        else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc)
            seed = strtoull(argv[++i], NULL, 10);
        else {
            fprintf(stderr,
                    "использование: %s [--duration МС] [--ledger ПУТЬ] [--seed N]\n",
                    argv[0]);
            return 2;
        }
    }

    tb_arch_init();
    if (seed == 0)
        seed = tb_rdtsc();
    tb_log_set_level(TB_LOG_INFO);

    printf("=================================================================\n");
    printf(" events_demo — модуль регистрации событий (итерация ядра v0.2)\n");
    printf(" длительность: %llu мс | seed: %llu | журнал: %s\n",
           (unsigned long long)duration_ms, (unsigned long long)seed,
           ledger_path);
    printf(" маркеры диагностики: stderr (отделяйте: 2>markers.log)\n");
    printf("=================================================================\n");

    demo_app_t d;
    memset(&d, 0, sizeof(d));
    d.ledger_path = ledger_path;

    tb_error_t err = TB_ERROR_INIT;

    /* Журнал и цикл. */
    d.ledger = tb_ledger_open(ledger_path, TB_LEDGER_FLUSH_LINE, &err);
    if (!d.ledger) {
        char line[256];
        tb_error_format(&err, line, sizeof(line));
        fprintf(stderr, "не удалось открыть журнал: %s\n", line);
        return 1;
    }
    d.loop = tb_loop_create(0, &err); /* пул не нужен: имитация полёта — таймер */
    if (!d.loop) {
        char line[256];
        tb_error_format(&err, line, sizeof(line));
        fprintf(stderr, "не удалось создать цикл: %s\n", line);
        tb_ledger_close(d.ledger);
        return 1;
    }
    g_loop = d.loop;
    signal(SIGINT, demo_sigint);
    signal(SIGTERM, demo_sigint);

    /* Политики источника (числа демонстрационные). */
    tb_ratelimit_init(&d.rl, 30, 8);          /* консервативнее публичного лимита */
    tb_breaker_init(&d.breaker, 3, 3000);
    tb_backoff_init(&d.backoff, 700, 5000, 0.25, seed);
    d.cache.ttl_ms = 8000;

    /* Реестр событий и цепочка шагов. */
    d.reg = tb_evreg_create(TB_EVENTS_DEFAULT_LIVE, &err);
    if (!d.reg) {
        char line[256];
        tb_error_format(&err, line, sizeof(line));
        fprintf(stderr, "не удалось создать реестр: %s\n", line);
        return 1;
    }
    tb_evreg_add(d.reg, "core", core_fn, NULL, NULL);
    tb_evreg_add(d.reg, "ratelimit", tb_evmod_ratelimit_fn,
                 &(tb_evmod_ratelimit_t){ .rl = &d.rl }, NULL);
    tb_evreg_add(d.reg, "breaker", tb_evmod_breaker_fn,
                 &(tb_evmod_breaker_t){ .br = &d.breaker }, NULL);
    tb_evreg_add(d.reg, "http", http_fn, &d, NULL);
    tb_evreg_add(d.reg, "http_resp", http_resp_fn, &d, NULL);

    /* Наблюдатели: мост к планировщику и мост к журналу. */
    tb_evreg_on_state(d.reg, 1u << TB_EV_BLOCKED, bridge_observer, &d);
    tb_ev_ledger_attach(d.reg, d.ledger);

    /* Расписание фаз (мс от старта). */
    uint64_t t0 = tb_now_ms();
    schedule_phase(&d, 0, t0 + 500);
    schedule_phase(&d, 1, t0 + 5000);
    schedule_phase(&d, 2, t0 + 13000);
    schedule_phase(&d, 3, t0 + 16000);
    schedule_phase(&d, 4, t0 + 20000);
    schedule_phase(&d, 5, t0 + 23000);
    schedule_phase(&d, 6, t0 + 26000);
    tb_loop_call_later(d.loop, duration_ms, TB_PRIO_CRITICAL, "stop",
                       demo_stop, &d, NULL);
    tb_loop_call_every(d.loop, 6000, TB_PRIO_LOW, "dump", dump_task, &d, NULL);

    /* Запуск в главном потоке. */
    int rc = tb_loop_run(d.loop);

    /* ---- Итоги ---- */
    const tb_evreg_stats_t *es = tb_evreg_stats(d.reg);
    const tb_ledger_stats_t *ls = tb_ledger_stats(d.ledger);
    const tb_loop_stats_t *lps = tb_loop_stats(d.loop);

    printf("\n======================= ИТОГИ ДЕМО =======================\n");
    printf("События: создано %llu | DONE %llu | FAILED %llu | CANCELED %llu\n",
           (unsigned long long)es->events_created,
           (unsigned long long)es->events_done,
           (unsigned long long)es->events_failed,
           (unsigned long long)es->events_canceled);
    printf("Прогоны: dispatch %llu | продолжений %llu | полных повторов и "
           "перестроек %llu | BLOCK-переходов %llu\n",
           (unsigned long long)es->dispatches,
           (unsigned long long)es->continues,
           (unsigned long long)es->restarts,
           (unsigned long long)es->blocked);
    printf("Live-таблица: осталось %zu (QUEUED %zu, BLOCKED %zu)\n",
           tb_evreg_count(d.reg, TB_EV_ANY_MASK),
           tb_evreg_count(d.reg, 1u << TB_EV_QUEUED),
           tb_evreg_count(d.reg, 1u << TB_EV_BLOCKED));
    printf("HTTP: отправок %llu | 200=%llu 202=%llu 429=%llu 5xx=%llu\n",
           (unsigned long long)d.http_sent,
           (unsigned long long)d.resp_ok,
           (unsigned long long)d.resp_pending,
           (unsigned long long)d.resp_429,
           (unsigned long long)d.resp_5xx);
    printf("Кэш: hits=%llu stores=%llu | Rate limit: granted=%llu deferred=%llu\n",
           (unsigned long long)d.cache.hits,
           (unsigned long long)d.cache.stores,
           (unsigned long long)d.rl.granted,
           (unsigned long long)d.rl.deferred);
    printf("Breaker: переходов %llu, отклонений %llu | Цикл: задач %llu таймеров %llu\n",
           (unsigned long long)d.breaker.transitions,
           (unsigned long long)d.breaker.rejected_total,
           (unsigned long long)lps->tasks_run,
           (unsigned long long)lps->timers_fired);
    printf("Журнал: событий %llu (в т.ч. event.trace — терминальные), байт %llu\n",
           (unsigned long long)ls->events_written,
           (unsigned long long)ls->bytes_written);
    printf("Журнал: %s\n", d.ledger_path);
    printf("===========================================================\n");

    tb_evreg_destroy(d.reg);
    tb_loop_destroy(d.loop);
    tb_ledger_close(d.ledger);
    return rc == 0 ? 0 : 1;
}
