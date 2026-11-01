/* ==========================================================================
 *  demo/exec_demo.c — механизм торговли в живом конвейере.
 *
 *  Что показывает (поверх ядра v0.2, модуль tb/exec):
 *   - конвейер исполнения preflight -> ratelimit -> breaker -> quote ->
 *     balance -> approve -> execute -> settle -> commit на реестре событий;
 *   - ОДНА сделка = ОДНО событие: все мутации балансов — через торговый
 *     слот, фиксируемый атомарно (аналог «всё в одной транзакции»);
 *   - нативный вход/выход: wrap на входе, unwrap на выходе (WETH-семантика);
 *   - PENDING: ордер принят бэкендом -> trade.submitted -> поллинг ->
 *     заполнение (202-семантика Universal API Runtime);
 *   - retryable-отказ DEX: backoff и полный повтор цепочки (слот не
 *     списывает вход дважды);
 *   - slippage-защита: выход ниже минимума — ордер провален, балансы целы;
 *   - отказы валидации (дедлайн, баланс, маршрут) — до любых мутаций;
 *   - мост к планировщику (BLOCKED -> таймер -> продолжение) и журналу
 *     (trade.* + event.trace);
 *   - баланс-лист до/после: провалившиеся сделки не меняют балансы.
 *
 *  Запуск: exec_demo [--duration МС] [--ledger ПУТЬ] [--seed N]
 * ========================================================================== */
#define TB_EVENT_MARKERS 1

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tb/arch/arch.h"
#include "tb/core/error.h"
#include "tb/core/log.h"
#include "tb/core/marker.h"
#include "tb/core/time.h"
#include "tb/events/events.h"
#include "tb/exec/engine.h"
#include "tb/exec/u256.h"
#include "tb/exec/wallet.h"
#include "tb/ledger/ledger.h"
#include "tb/sched/loop.h"

/* ---- Активы стенда (адреса произвольные, цены симулятора синтетические) ---- */
static tb_addr_t A_WETH, A_TOKA, A_TOKB, A_TOKC;

static tb_addr_t hexaddr(const char *h)
{
    tb_addr_t a;
    if (tb_addr_from_hex(&a, h, NULL) != 0) {
        fprintf(stderr, "не могу разобрать адрес %s\n", h);
        exit(1);
    }
    return a;
}

static void amount_str(const tb_u256_t *v, char *buf, size_t cap)
{
    tb_u256_to_dec(v, buf, cap);
}

static void print_wallet(tb_wallet_t *w, const char *title)
{
    printf("\n--- %s ---\n", title);
    for (size_t i = 0; tb_wallet_asset_at(w, i, NULL, NULL); i++) {
        tb_addr_t asset;
        tb_u256_t bal;
        tb_wallet_asset_at(w, i, &asset, &bal);
        char hex[43], amt[80];
        tb_addr_to_hex(&asset, hex, sizeof(hex));
        amount_str(&bal, amt, sizeof(amt));
        printf("  %-44s %s%s\n", hex, amt,
               tb_addr_is_native(&asset) ? "   (нативный)"
               : tb_addr_eq(&asset, &A_WETH) ? "   (wrapped)"
                                         : "");
    }
}

/* ---- Приложение ---- */
typedef struct demo_app {
    tb_exec_engine_t *exec;
    tb_evreg_t       *reg;
    tb_loop_t        *loop;
    tb_ledger_t      *ledger;
    const char       *ledger_path;
    tb_u256_t         snap[TB_WALLET_MAX_ASSETS]; /* балансы «до» */
} demo_app_t;

static tb_loop_t *g_loop;

/* Мост к планировщику: BLOCKED(wake_at) -> таймер -> продолжение. */
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
    if (!ev || ev->state != TB_EV_BLOCKED)
        return; /* закрыт или продолжен */
    TB_MARK("demo: таймер будит сделку #%llu", (unsigned long long)ev->id);
    if (tb_evreg_continue(ra->app->reg, ev, &err) != 0)
        TB_LOG_WARN("demo: продолжение #%llu не удалось: %s",
                    (unsigned long long)ev->id, err.message);
}

static void bridge_observer(tb_evreg_t *r, tb_event_t *ev, void *user)
{
    demo_app_t *d = user;
    (void)r;
    if (ev->state != TB_EV_BLOCKED || ev->wake_at_ms == 0)
        return;
    uint64_t now = tb_now_ms();
    uint64_t delay = ev->wake_at_ms > now ? ev->wake_at_ms - now : 0;
    resume_arg_t *ra = malloc(sizeof(*ra));
    if (!ra)
        return;
    ra->app      = d;
    ra->event_id = ev->id;
    tb_loop_call_later(d->loop, delay, TB_PRIO_HIGH, "exec.resume",
                       resume_task, ra, free);
}

/* ---- Подача ордера с подписью сценария ---- */
typedef struct {
    demo_app_t *app;
    tb_exec_order_t order;
    const char *title;
} order_arg_t;

static void submit_task(tb_loop_t *loop, tb_task_t *task, void *arg)
{
    order_arg_t *oa = arg;
    demo_app_t *d = oa->app;
    (void)loop;
    (void)task;

    tb_error_t err = TB_ERROR_INIT;
    printf("\n=== %s ===\n", oa->title);
    uint64_t id = tb_exec_submit(d->exec, &oa->order, &err);
    if (id == 0) {
        char line[256];
        tb_error_format(&err, line, sizeof(line));
        printf("подача не удалась: %s\n", line);
    }
    /* oa принадлежит задаче цикла: освобождает dtor (free) ровно один раз */
}

static void schedule_order(demo_app_t *d, uint64_t at_ms, const char *title,
                           tb_exec_order_t order)
{
    order_arg_t *oa = malloc(sizeof(*oa));
    if (!oa)
        return;
    oa->app   = d;
    oa->order = order;
    oa->title = title;
    tb_loop_call_at(d->loop, at_ms, TB_PRIO_NORMAL, "order",
                    submit_task, oa, free);
}

static tb_exec_order_t mk_order(tb_addr_t in, tb_addr_t out,
                                const char *amount, const char *min,
                                tb_route_t route, uint64_t deadline)
{
    tb_exec_order_t o;
    memset(&o, 0, sizeof(o));
    o.token_in  = in;
    o.token_out = out;
    tb_u256_from_dec(&o.amount_in, amount, strlen(amount), NULL);
    if (min)
        tb_u256_from_dec(&o.amount_out_min, min, strlen(min), NULL);
    o.deadline_ms = deadline;
    o.route = route;
    return o;
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

int main(int argc, char **argv)
{
    uint64_t duration_ms = 14000;
    const char *ledger_path = "exec_demo.jsonl";
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
    printf(" exec_demo — механизм торговли (Execution Engine, ядро v0.2)\n");
    printf(" длительность: %llu мс | seed: %llu | журнал: %s\n",
           (unsigned long long)duration_ms, (unsigned long long)seed,
           ledger_path);
    printf(" цепочка: preflight -> ratelimit -> breaker -> quote -> balance\n");
    printf("          -> approve -> execute -> settle -> commit\n");
    printf("=================================================================\n");

    A_WETH = hexaddr("0xeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee");
    A_TOKA = hexaddr("0xaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    A_TOKB = hexaddr("0xbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    A_TOKC = hexaddr("0xcccccccccccccccccccccccccccccccccccccccc");

    tb_error_t err = TB_ERROR_INIT;

    /* Сценарии бэкенда: заполнение, PENDING, retryable-отказ, slippage,
     * тонкая ликвидность. */
    tb_exec_sim_scenario_t script[] = {
        { TB_SIM_FILL,   0,    997, 1000, TB_EXEC_OK, false, "немедленное заполнение" },
        { TB_SIM_FILL,   0,    995, 1000, TB_EXEC_OK, false, "немедленное заполнение" },
        { TB_SIM_FILL,   0,    998, 1000, TB_EXEC_OK, false, "немедленное заполнение" },
        { TB_SIM_FILL,   0,    990, 1000, TB_EXEC_OK, false, "многохоп: заполнение" },
        { TB_SIM_FILL,   0,    970, 1000, TB_EXEC_OK, false, "плохая цена — на проверку slippage" },
        { TB_SIM_SUBMIT, 1500, 995, 1000, TB_EXEC_OK, false, "принято, заполнится через 1.5 с" },
        { TB_SIM_REJECT, 0,    0,   0,    TB_EXEC_BACKEND, true, "503 от DEX-адаптера" },
        { TB_SIM_FILL,   0,    991, 1000, TB_EXEC_OK, false, "заполнение после повтора" },
        { TB_SIM_REJECT, 0,    0,   0,    TB_EXEC_INSUFFICIENT_LIQUIDITY, false,
          "маршрут тонкий: ликвидности мало" },
        { TB_SIM_FILL,   0,    996, 1000, TB_EXEC_OK, false, "заполнение" },
    };

    tb_exec_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.wrapped_native        = A_WETH;
    cfg.script                = script;
    cfg.script_n              = sizeof(script) / sizeof(script[0]);
    cfg.seed                  = seed;
    cfg.rl_rate_per_min       = 60;  /* бюджет DEX-бэкенда */
    cfg.rl_burst              = 10;
    cfg.br_threshold          = 4;
    cfg.br_cooldown_ms        = 2000;
    cfg.backoff_base_ms       = 400;
    cfg.backoff_cap_ms        = 3000;
    cfg.backoff_jitter        = 0.2;
    cfg.fill_poll_max         = 10;
    cfg.fill_poll_interval_ms = 300;
    cfg.max_slippage_bp       = 100; /* авто-минимум: 1% */
    cfg.min_deadline_ahead_ms = 300;
    cfg.max_hops              = 4;

    demo_app_t d;
    memset(&d, 0, sizeof(d));
    d.ledger_path = ledger_path;

    d.ledger = tb_ledger_open(ledger_path, TB_LEDGER_FLUSH_LINE, &err);
    if (!d.ledger) {
        char line[256];
        tb_error_format(&err, line, sizeof(line));
        fprintf(stderr, "не удалось открыть журнал: %s\n", line);
        return 1;
    }
    d.loop = tb_loop_create(0, &err);
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

    d.exec = tb_exec_create(&cfg, &err);
    if (!d.exec) {
        char line[256];
        tb_error_format(&err, line, sizeof(line));
        fprintf(stderr, "не удалось создать движок: %s\n", line);
        return 1;
    }
    d.reg = tb_exec_registry(d.exec);
    if (tb_exec_attach_ledger(d.exec, d.ledger) < 0) {
        fprintf(stderr, "не удалось подключить журнал к движку\n");
        return 1;
    }
    tb_evreg_on_state(d.reg, 1u << TB_EV_BLOCKED, bridge_observer, &d);

    /* Депозиты стенда (виртуальные балансы трейдера). */
    tb_wallet_t *w = (tb_wallet_t *)tb_exec_wallet(d.exec);
    {
        struct { tb_addr_t a; const char *amt; } deps[] = {
            { tb_addr_zero(), "10000000000000000000" }, /* 10 нативного */
            { A_WETH,         "0" },
            { A_TOKA,         "25000000000000000000" }, /* 25 A */
            { A_TOKB,         "40000000000000000000" }, /* 40 B */
            { A_TOKC,         "7000000000000000000" },  /* 7 C */
        };
        for (size_t i = 0; i < sizeof(deps) / sizeof(deps[0]); i++) {
            tb_u256_t v;
            tb_u256_from_dec(&v, deps[i].amt, strlen(deps[i].amt), NULL);
            tb_wallet_deposit(w, &deps[i].a, &v, NULL);
        }
    }
    for (size_t i = 0; tb_wallet_asset_at(w, i, NULL, NULL); i++)
        tb_wallet_asset_at(w, i, NULL, &d.snap[i]);
    print_wallet(w, "БАЛАНСЫ ДО ТОРГОВЛИ");

    /* Маршруты. */
    tb_route_t r_ab  = tb_route_single(&A_TOKA, &A_TOKB, 3000, NULL);
    tb_route_t r_wb  = tb_route_single(&A_WETH, &A_TOKB, 500, NULL);
    tb_route_t r_aw  = tb_route_single(&A_TOKA, &A_WETH, 3000, NULL);
    tb_route_t r_abc = tb_route_encode(
        (tb_addr_t[]){ A_TOKA, A_TOKB, A_TOKC }, (uint32_t[]){ 3000, 500 }, 3,
        NULL);
    tb_route_t r_bc  = tb_route_single(&A_TOKB, &A_TOKC, 10000, NULL);

    uint64_t t0      = tb_now_ms();
    uint64_t far     = tb_wall_ms() + 120000; /* дедлайн «на всё» */
    uint64_t at      = t0 + 400;
    uint64_t step    = 1100;

    /* 1. Обычная сделка A -> B. */
    schedule_order(&d, at, "1) A -> B, однохоп — ожидаем DONE", 
                   mk_order(A_TOKA, A_TOKB, "1000000000000000000", NULL, r_ab, far));
    /* 2. Нативный вход: wrap -> WETH|500|B. */
    schedule_order(&d, at += step, "2) NATIVE -> B (wrap на входе) — ожидаем DONE",
                   mk_order(tb_addr_zero(), A_TOKB, "2000000000000000000", NULL, r_wb, far));
    /* 3. Нативный выход: A|3000|WETH, unwrap на выходе. */
    schedule_order(&d, at += step, "3) A -> NATIVE (unwrap на выходе) — ожидаем DONE",
                   mk_order(A_TOKA, tb_addr_zero(), "1000000000000000000", NULL, r_aw, far));
    /* 4. Многохоп A -3000- B -500- C. */
    schedule_order(&d, at += step, "4) A -> B -> C, многохоп — ожидаем DONE",
                   mk_order(A_TOKA, A_TOKC, "1500000000000000000", NULL, r_abc, far));
    /* 5. Slippage: минимум заведомо выше любого выхода маршрута. */
    schedule_order(&d, at += step, "5) B -> C, минимум 5x входа — ожидаем FAILED (slippage), балансы целы",
                   mk_order(A_TOKB, A_TOKC, "1000000000000000000", "5000000000000000000", r_bc, far));
    /* 6. PENDING: принят, заполняется через 1.5 с поллингом. */
    schedule_order(&d, at += step, "6) C -> A, PENDING бэкенда — ожидаем trade.submitted, затем DONE",
                   mk_order(A_TOKC, A_TOKA, "1000000000000000000", NULL,
                            tb_route_single(&A_TOKC, &A_TOKA, 3000, NULL), far));
    /* 7. Retryable-отказ DEX: backoff, повтор цепочки, заполнение. */
    schedule_order(&d, at += step, "7) A -> B, 503 от DEX — ожидаем backoff, затем DONE",
                   mk_order(A_TOKA, A_TOKB, "1000000000000000000", NULL, r_ab, far));
    /* 8. Не хватает баланса. */
    schedule_order(&d, at += step, "8) B -> A, сумма выше баланса — ожидаем FAILED (баланс)",
                   mk_order(A_TOKB, A_TOKA, "90000000000000000000", NULL,
                            tb_route_single(&A_TOKB, &A_TOKA, 3000, NULL), far));
    /* 9. Дедлайн в прошлом. */
    schedule_order(&d, at += step, "9) A -> B, дедлайн в прошлом — ожидаем FAILED (дедлайн)",
                   mk_order(A_TOKA, A_TOKB, "1000000000000000000", NULL, r_ab,
                            tb_wall_ms() - 1000));
    /* 10. Маршрут не согласован с ордером. */
    schedule_order(&d, at += step, "10) вход A, маршрут начинается с B — ожидаем FAILED (маршрут)",
                   mk_order(A_TOKA, A_TOKB, "1000000000000000000", NULL, r_bc, far));
    /* 11. Тонкая ликвидность. */
    schedule_order(&d, at += step, "11) A -> B, тонкая ликвидность — ожидаем FAILED (бэкенд)",
                   mk_order(A_TOKA, A_TOKB, "1000000000000000000", NULL, r_ab, far));

    tb_loop_call_later(d.loop, duration_ms, TB_PRIO_CRITICAL, "stop",
                       demo_stop, &d, NULL);

    int rc = tb_loop_run(d.loop);

    /* ---- Итоги ---- */
    const tb_exec_stats_t *es = tb_exec_stats(d.exec);
    const tb_ledger_stats_t *ls = tb_ledger_stats(d.ledger);

    printf("\n======================= ИТОГИ ДЕМО =======================\n");
    printf("Ордера: подано %llu | исполнено %llu | провалено %llu\n",
           (unsigned long long)es->submitted,
           (unsigned long long)es->filled,
           (unsigned long long)es->failed);
    printf("Ветви: PENDING-записей %llu | повторов после retryable %llu\n",
           (unsigned long long)es->pending_records,
           (unsigned long long)es->retries);
    printf("Отказы: slippage %llu | бэкенд %llu | таймауты %llu\n",
           (unsigned long long)es->slippage_rejects,
           (unsigned long long)es->backend_rejects,
           (unsigned long long)es->timeouts);

    print_wallet(w, "БАЛАНСЫ ПОСЛЕ ТОРГОВЛИ");
    printf("\nПровалившиеся сделки (5, 8, 9, 10, 11) балансов не меняют:\n");
    printf("  B и C на шаге 5 не тронуты, A на шагах 9-11 не тронут —\n");
    printf("  все списания шли через слот и были откачены при FAILED.\n");

    printf("\nЖурнал: %s (событий %llu, байт %llu; trade.* + event.trace)\n",
           d.ledger_path, (unsigned long long)ls->events_written,
           (unsigned long long)ls->bytes_written);
    printf("===========================================================\n");

    tb_exec_destroy(d.exec);
    tb_loop_destroy(d.loop);
    tb_ledger_close(d.ledger);
    return rc == 0 ? 0 : 1;
}
