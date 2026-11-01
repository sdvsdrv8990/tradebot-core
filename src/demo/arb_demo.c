#define TB_CURRENT_GROUP TB_G_DEMO /* зона ответственности файла */
/* ==========================================================================
 *  tb/demo/arb_demo.c — демо арбитражного конвейера (v0.3).
 *
 *  Режимы:
 *    по умолчанию — ЖИВОЙ: котировки Uniswap V3 mainnet через публичный
 *      RPC (без ключа), исполнение shadow на виртуальных балансах;
 *    --replay     — офлайн: локальный mock-RPC (127.0.0.1, обычный HTTP)
 *      с синтетическим рынком со спредом между fee-тирами; сделка
 *      гарантированно находится и проходит полный конвейер.
 *
 *  Прогон: config -> движок (инъекция DEX-бэкенда) -> кошелёк -> конвейер
 *  tb_arb (фильтры 1-4 -> Decision Engine -> tb_exec_submit) -> монитор
 *  отмены. Итог: статистика + виртуальный PnL + журнал decision.* /
 *  trade.* / event.trace в JSONL.
 *
 *    ./arb_demo [--config docs/arb_config.example.json] [--ledger ./arb_demo.jsonl]
 *               [--duration МС (0=бессрочно, дефолт)] [--replay] [--live-exec]
 *               [--web-port N] [--no-web]
 * ========================================================================== */
#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "tb/arch/arch.h"
#include "tb/core/time.h"
#include "tb/exec/backend_dex.h"
#include "tb/exec/safe_exec.h"
#include "tb/sign/tx.h"
#include "tb/exec/engine.h"
#include "tb/exec/wallet.h"
#include "tb/ledger/ledger.h"
#include "tb/sched/loop.h"
#include "tb/user/arb.h"
#include "tb/user/webui.h"

/* ---- mock-RPC: синтетический рынок для --replay ---- */

typedef struct mock_rpc {
    int       fd;
    int       port;
    pthread_t th;
    bool      stop;
} mock_rpc_t;

#define WETH_HEX "c02aaa39b223fe8d0a0e5c4f27ead9083c756cc2"
#define USDC_HEX "a0b86991c6218b36c1d19d4a2e9eb0ce3606eb48"
#define USDT_HEX "dac17f958d2ee523a2206206994597c13d831ec7"

/* Ставка: ETH = 2263 USD; fee-тир 500 — «дёшево» (+0.35%), 3000 —
 * «дорого» (−0.35%): спред пары ~0.7% за цикл. Влияние размера
 * линейно и мягкое (1e18 входа ≈ −33%). */
static unsigned __int128 mock_rate(const char *token_in_hex, uint64_t fee)
{
    unsigned __int128 p;
    if (strncmp(token_in_hex, USDC_HEX, 40) == 0
        || strncmp(token_in_hex, USDT_HEX, 40) == 0)
        p = 1000000000000000000ull; /* обратное направление: 1/P ниже */
    else
        p = 2263000000000000000ull; /* 2263.0 в 1e15 */
    (void)p;
    /* возвращает модификатор направления и тира вызывающий */
    if (fee == 500)
        return 1003500000000000ull; /* 1.0035 в 1e15 */
    if (fee == 3000)
        return 996500000000000ull; /* 0.9965 в 1e15 */
    return 1000000000000000ull;
}

/* Вычислить amountOut по calldata QuoterV2. */
static uint64_t mock_quote(const char *data_hex)
{
    /* 0x + selector(8) + tokenIn(64) + tokenOut(64) + amount(64) + fee */
    const char *w_in = data_hex + 10;
    const char *w_out = data_hex + 10 + 64;
    const char *w_amt = data_hex + 10 + 128;
    const char *w_fee = data_hex + 10 + 192;

    char in_tok[41], out_tok[41];
    memcpy(in_tok, w_in + 24, 40);
    in_tok[40] = 0;
    memcpy(out_tok, w_out + 24, 40);
    out_tok[40] = 0;

    /* Парсинг строго по границам слов: strtoull без границы пробежал бы
     * через конец слова в следующие hex-цифры (fee-слово, нули). */
    char num[32];
    memcpy(num, w_amt, 16);
    num[16] = '\0';
    if (strtoull(num, NULL, 16)) /* старшая четверть слова: > 2^192 */
        return 0;
    memcpy(num, w_amt + 48, 16);
    num[16] = '\0';
    uint64_t amt = strtoull(num, NULL, 16);
    memcpy(num, w_fee + 58, 6);
    num[6] = '\0';
    uint32_t fee = (uint32_t)strtoul(num, NULL, 16);

    bool in_is_weth = strncmp(in_tok, WETH_HEX, 40) == 0;
    bool out_is_weth = strncmp(out_tok, WETH_HEX, 40) == 0;
    /* Тир-модификатор: покупка на «дешёвом» пуле (fee 500) даёт больше
     * токенов; продажа на «дорогом» (fee 3000) — больше wei. Направления
     * инверсны, поэтому круг через один пул всегда в минус (спред+влияние),
     * а через два разных — с прибылью ~0.7%. */
    unsigned __int128 mod;
    if (in_is_weth && !out_is_weth)
        mod = (fee == 500) ? 1003500000000000ull
                           : (fee == 3000) ? 996500000000000ull
                                           : 1000000000000000ull;
    else if (!in_is_weth && out_is_weth)
        mod = (fee == 3000) ? 1003500000000000ull
                            : (fee == 500) ? 996500000000000ull
                                           : 1000000000000000ull;
    else
        return 0;
    unsigned __int128 out;

    if (in_is_weth) {
        /* покупка: USDC = wei × P × mod / 1e12 (18 -> 6 знаков) */
        out = (unsigned __int128)amt * 2263ull * mod / 1000000000000ull
              / 1000000000000000ull;
        out = out * (3000000000000000000ull - amt)
              / 3000000000000000000ull; /* влияние размера */
    } else {
        /* продажа: wei = usd × 1e12 × mod / P */
        unsigned __int128 p = 2263ull * 1000000000000000ull;
        out = (unsigned __int128)amt * 1000000000000ull * mod / p;
        out = out * (3000000000000000ull - amt) / 3000000000000000ull;
    }

    if (out >> 64)
        return 0;
    return (uint64_t)out;
}

static void *mock_thread(void *arg)
{
    mock_rpc_t *m = arg;
    while (!tb_atomic_load_bool(&m->stop)) {
        int c = accept(m->fd, NULL, NULL);
        if (c < 0)
            break;

        char req[4096] = {0};
        size_t have = 0;
        while (have < sizeof(req) - 1) {
            ssize_t n = recv(c, req + have, sizeof(req) - 1 - have, 0);
            if (n <= 0)
                break;
            have += (size_t)n;
            if (strstr(req, "\r\n\r\n"))
                break;
        }
        const char *body = strstr(req, "\r\n\r\n");
        if (!body) {
            close(c);
            continue;
        }
        body += 4;

        const char *d = strstr(body, "\"data\":\"0x");
        char result[131] = "0x";
        memset(result + 2, '0', 128);
        result[130] = '\0';
        uint64_t out = 0;
        int ok = 0;
        if (d) {
            out = mock_quote(d + 8);
            ok = 1;
            for (int i = 0; i < 16; i++) { /* слово 0: значение
                                             * выровнено ВПРАВО (ABI) */
                int digit = (int)((out >> ((15 - i) * 4)) & 0xf);
                result[2 + 48 + i] = "0123456789abcdef"[digit];
            }
        }
        char resp[512];
        int n;
        if (ok)
            n = snprintf(resp, sizeof(resp),
                         "HTTP/1.1 200 OK\r\nContent-Type: "
                         "application/json\r\nConnection: close\r\n\r\n"
                         "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":\"%s\"}",
                         result);
        else
            n = snprintf(resp, sizeof(resp),
                         "HTTP/1.1 200 OK\r\nContent-Type: "
                         "application/json\r\nConnection: close\r\n\r\n"
                         "{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":"
                         "{\"code\":3,\"message\":\"no pool\"}}");
        send(c, resp, (size_t)n, 0);
        close(c);
    }
    return NULL;
}

static void mock_start(mock_rpc_t *m)
{
    tb_atomic_store_bool(&m->stop, false);
    m->fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    bind(m->fd, (struct sockaddr *)&a, sizeof(a));
    listen(m->fd, 16);
    struct sockaddr_in got = {0};
    socklen_t gl = sizeof(got);
    getsockname(m->fd, (struct sockaddr *)&got, &gl);
    m->port = ntohs(got.sin_port);
    struct timeval tv = { .tv_sec = 5 };
    setsockopt(m->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    pthread_create(&m->th, NULL, mock_thread, m);
}

static void mock_stop(mock_rpc_t *m)
{
    tb_atomic_store_bool(&m->stop, true);
    /* shutdown будит accept() в потоке сервера (close — нет), close после join */
    shutdown(m->fd, SHUT_RDWR);
    pthread_join(m->th, NULL);
    close(m->fd);
}

/* ---- приложение ---- */

typedef struct app {
    tb_loop_t         *loop;
    tb_evreg_t        *reg;   /* реестр движка */
    mock_rpc_t         mock;
    bool               replay;
    tb_webui_t        *web;
    tb_arb_t          *arb;
    tb_exec_engine_t  *engine;
    tb_u256_t          start_weth;
    const tb_arb_cfg_t *cfg;
    uint64_t           started_ms;
} app_t;

static app_t g_app;

static void on_sigint(int sig)
{
    (void)sig;
    if (g_app.loop)
        tb_loop_stop(g_app.loop); /* как в bot_demo: из обработчика можно */
}

static void stop_task(tb_loop_t *loop, tb_task_t *task, void *arg)
{
    (void)task;
    (void)arg;
    tb_loop_stop(loop);
}


static void web_update_task(tb_loop_t *loop, tb_task_t *task, void *arg)
{
    (void)loop; (void)task;
    app_t *d = arg;
    if (!d->web)
        return;
    const tb_arb_stats_t *as = tb_arb_stats(d->arb);
    const tb_exec_stats_t *es = tb_exec_stats(d->engine);
    tb_wallet_t *w = (tb_wallet_t *)tb_exec_wallet(d->engine);
    tb_u256_t bal = tb_wallet_balance(w, &d->cfg->weth);
    tb_u256_t pnl;
    bool profit = tb_u256_sub(&pnl, &bal, &d->start_weth);
    char bal_s[80], pnl_s[80];
    tb_u256_to_dec(&bal, bal_s, sizeof(bal_s));
    tb_u256_t zero = tb_u256_from_u64(0);
    tb_u256_to_dec(profit ? &pnl : &zero, pnl_s, sizeof(pnl_s));
    char mode[24];
    snprintf(mode, sizeof(mode), "%s%s",
             d->replay ? "replay" : "live-котировки",
             d->cfg->dex.live ? " + Safe" : "");
    char json[2048];
    snprintf(json, sizeof(json),
             "{\"mode\":\"%s\",\"uptime_ms\":%llu,\"cards\":["
             "{\"t\":\"опросы\",\"v\":\"%llu\"},"
             "{\"t\":\"котировок\",\"v\":\"%llu\"},"
             "{\"t\":\"отсеяно фильтрами\",\"v\":\"%llu\"},"
             "{\"t\":\"пар оценено\",\"v\":\"%llu\"},"
             "{\"t\":\"выгодных\",\"v\":\"%llu\"},"
             "{\"t\":\"ордеров подано\",\"v\":\"%llu\"},"
             "{\"t\":\"filled\",\"v\":\"%llu\",\"cls\":\"good\"},"
             "{\"t\":\"failed\",\"v\":\"%llu\",\"cls\":\"bad\"},"
             "{\"t\":\"canceled\",\"v\":\"%llu\"},"
             "{\"t\":\"ошибок котировщика\",\"v\":\"%llu\"},"
             "{\"t\":\"кошелёк WETH wei\",\"v\":\"%s\"},"
             "{\"t\":\"PnL wei%s\",\"v\":\"%s%s\",\"cls\":\"%s\"}]}",
             mode,
             (unsigned long long)(tb_now_ms() - d->started_ms),
             (unsigned long long)as->polls,
             (unsigned long long)as->quotes,
             (unsigned long long)as->filter_rejects,
             (unsigned long long)as->pairs_evaluated,
             (unsigned long long)as->pairs_profitable,
             (unsigned long long)as->orders_submitted,
             (unsigned long long)es->filled,
             (unsigned long long)es->failed,
             (unsigned long long)es->canceled,
             (unsigned long long)as->quote_errors,
             bal_s,
             profit ? "" : " (−)",
             profit ? "" : "-", pnl_s,
             profit ? "good" : "bad");
    tb_webui_set_json(d->web, json);
}

typedef struct resume_arg {
    app_t    *app;
    uint64_t  event_id;
} resume_arg_t;

static void resume_task(tb_loop_t *loop, tb_task_t *task, void *arg)
{
    (void)loop;
    (void)task;
    resume_arg_t *ra = arg;
    tb_error_t err = TB_ERROR_INIT;
    tb_event_t *ev = tb_evreg_find(ra->app->reg, ra->event_id);
    if (!ev || ev->state != TB_EV_BLOCKED)
        return;
    if (tb_evreg_continue(ra->app->reg, ev, &err) != 0)
        fprintf(stderr, "resume #%llu: %s\n",
                (unsigned long long)ra->event_id, err.message);
}

static void bridge_observer(tb_evreg_t *r, tb_event_t *ev, void *user)
{
    app_t *d = user;
    (void)r;
    if (ev->state != TB_EV_BLOCKED || ev->wake_at_ms == 0)
        return;
    uint64_t now = tb_now_ms();
    uint64_t delay = ev->wake_at_ms > now ? ev->wake_at_ms - now : 0;
    resume_arg_t *ra = malloc(sizeof(*ra));
    if (!ra)
        return;
    ra->app = d;
    ra->event_id = ev->id;
    tb_loop_call_later(d->loop, delay, TB_PRIO_HIGH, "exec.resume",
                       resume_task, ra, free);
}

static void eth_str(const tb_u256_t *wei, char *buf, size_t cap)
{
    char d[80];
    tb_u256_to_dec(wei, d, sizeof(d));
    /* до 18 знаков — целые ETH, дальше дробь */
    size_t len = strlen(d);
    if (len <= 18) {
        snprintf(buf, cap, "0.%0*d%s", (int)(18 - len), 0, d);
        return;
    }
    snprintf(buf, cap, "%.*s.%s", (int)(len - 18), d, d + len - 18);
}

int main(int argc, char **argv)
{
    const char *cfg_path = "docs/arb_config.example.json";
    const char *ledger_path = "./arb_demo.jsonl";
    uint64_t duration_ms = 0; /* 0 = без ограничения по времени */
    bool replay = false;
    bool live_exec = false;
    bool print_addr = false;
    long web_port = 8080;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc)
            cfg_path = argv[++i];
        else if (strcmp(argv[i], "--ledger") == 0 && i + 1 < argc)
            ledger_path = argv[++i];
        else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc)
            duration_ms = strtoull(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--replay") == 0)
            replay = true;
        else if (strcmp(argv[i], "--live-exec") == 0)
            live_exec = true;
        else if (strcmp(argv[i], "--print-addr") == 0)
            print_addr = true;
        else if (strcmp(argv[i], "--web-port") == 0 && i + 1 < argc)
            web_port = strtol(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--no-web") == 0)
            web_port = 0;
        else {
            fprintf(stderr,
                    "использование: %s [--config ФАЙЛ] [--ledger ФАЙЛ] "
                    "[--duration МС (0=бессрочно)] [--replay] [--live-exec] "
                    "[--print-addr] [--web-port N] [--no-web]\n",
                    argv[0]);
            return 2;
        }
    }

    setvbuf(stdout, NULL, _IOLBF, 0); /* построчно: демо читают через пайп */
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_sigint);  /* Ctrl+C из терминала */
    signal(SIGTERM, on_sigint); /* стандартная остановка Linux: kill, systemctl */
    tb_error_t err = TB_ERROR_INIT;

    /* ---- конфигурация ---- */
    tb_arb_cfg_t cfg;
    if (tb_arb_cfg_load(&cfg, cfg_path, &err) != 0) {
        fprintf(stderr, "конфиг '%s': %s\n", cfg_path, err.message);
        return 1;
    }

    /* ---- режим исполнения: конфиг network.live_exec или флаг ---- */
    if (cfg.dex.live)
        live_exec = true;
    if (replay)
        live_exec = false; /* replay — офлайн-демо, исполнения не бывает */

    /* Safe-вызовы ходят в RPC первого venue, если не задан отдельный */
    if (!cfg.dex.safe.rpc_url[0] && cfg.dex.venues_n > 0) {
        snprintf(cfg.dex.safe.rpc_url, sizeof(cfg.dex.safe.rpc_url), "%s",
                 cfg.dex.venues[0].rpc_url);
        snprintf(cfg.dex.safe.rpc_host_pinned,
                 sizeof(cfg.dex.safe.rpc_host_pinned), "%s",
                 cfg.dex.venues[0].rpc_host_pinned);
    }

    /* ---- ключ бота: env TB_BOT_KEY или network.key_file ---- */
    if (live_exec || print_addr) {
        tb_key_t key;
        if (tb_key_from_env(&key, "TB_BOT_KEY", NULL) != 0) {
            /* файл: одна строка hex (перевод строки обрезаем) */
            char hex[128] = {0};
            FILE *kf = fopen(cfg.key_file[0] ? cfg.key_file : "bot_key.hex",
                             "r");
            if (!kf) {
                fprintf(stderr,
                        "ключ: задайте TB_BOT_KEY или создайте файл:\n"
                        "  openssl rand -hex 32 > %s\n",
                        cfg.key_file[0] ? cfg.key_file : "bot_key.hex");
                return 1;
            }
            if (!fgets(hex, sizeof(hex), kf)) {
                fprintf(stderr, "ключ: пустой файл\n");
                fclose(kf);
                return 1;
            }
            fclose(kf);
            hex[strcspn(hex, " \t\r\n")] = '\0';
            if (tb_key_from_hex(&key, hex, &err) != 0) {
                fprintf(stderr, "ключ: %s\n", err.message);
                return 1;
            }
        }
        cfg.dex.safe.key = key;

        char addr_hex[43];
        tb_addr_to_hex(&key.address, addr_hex, sizeof(addr_hex));
        printf("ключ бота: %s\n", addr_hex);
        if (print_addr)
            return 0; /* только адрес — для добавления в Safe */
    }

    app_t app = {0};
    app.replay = replay;

    if (replay) {
        /* офлайн: локальный mock-RPC + демо-параметры экономии */
        mock_start(&app.mock);
        snprintf(cfg.dex.venues[0].rpc_url,
                 sizeof(cfg.dex.venues[0].rpc_url),
                 "http://127.0.0.1:%u", (unsigned)app.mock.port);
        cfg.poll_ms = 1200;
        cfg.probe_wei = 100000000000000ull;
        cfg.min_trade_wei = 100000000000000ull;
        cfg.max_trade_wei = 4000000000000000ull;
        cfg.min_profit_wei = 100000000000ull;
        cfg.gas_est_wei = 400000000000ull;
        cfg.deadline_ms = 4000;
        cfg.dex.fill_delay_min_ms = 150;
        cfg.dex.fill_delay_max_ms = 300;
        printf("режим replay: mock-RPC 127.0.0.1:%u, спред тиров ~0.7%%\n",
               (unsigned)app.mock.port);
    }

    /* ---- транспорт ---- */
    tb_http_t *http = tb_http_net_create("tradebot-core/0.3", NULL, &err);
    if (!http) {
        fprintf(stderr, "транспорт: %s\n", err.message);
        return 1;
    }

    /* ---- бэкенд исполнения (движок); котировщик — свой внутри tb_arb ---- */
    tb_exec_backend_t dexb_exec;
    if (tb_exec_dex_init(&dexb_exec, http, &cfg.dex, &err) != 0) {
        fprintf(stderr, "dex-бэкенд: %s\n", err.message);
        return 1;
    }

    /* ---- движок ---- */
    tb_exec_cfg_t ecfg = {0};
    ecfg.wrapped_native = cfg.weth;
    ecfg.backend = &dexb_exec; /* инъекция вместо симулятора */
    ecfg.max_amount_in = tb_u256_from_u64(cfg.max_trade_wei);
    ecfg.allow[0] = cfg.weth;
    for (uint32_t i = 0; i < cfg.pools_n && ecfg.allow_n < TB_EXEC_ALLOW_MAX - 1;
         i++) {
        bool dup = false;
        for (uint32_t k = 0; k < ecfg.allow_n; k++)
            if (tb_addr_eq(&ecfg.allow[k], &cfg.pools[i].token))
                dup = true;
        if (!dup)
            ecfg.allow[1 + ecfg.allow_n++] = cfg.pools[i].token;
    }
    ecfg.allow_n++; /* +WETH */
    ecfg.rl_rate_per_min = 240;
    ecfg.rl_burst = 60;
    ecfg.br_threshold = 5;
    ecfg.br_cooldown_ms = 1500;
    ecfg.backoff_base_ms = 200;
    ecfg.backoff_cap_ms = 1200;
    ecfg.fill_poll_max = live_exec ? 120 : 6;          /* реальная tx:
                                                          до 2 минут */
    ecfg.fill_poll_interval_ms = live_exec ? 1000 : 250;
    ecfg.max_slippage_bp = 150;
    ecfg.min_deadline_ahead_ms = 200;

    tb_exec_engine_t *engine = tb_exec_create(&ecfg, &err);
    if (!engine) {
        fprintf(stderr, "движок: %s\n", err.message);
        return 1;
    }
    app.reg = tb_exec_registry(engine);

    /* ---- журнал ---- */
    remove(ledger_path);
    tb_ledger_t *ledger =
        tb_ledger_open(ledger_path, TB_LEDGER_FLUSH_LINE, &err);
    if (!ledger) {
        fprintf(stderr, "журнал: %s\n", err.message);
        return 1;
    }
    tb_exec_attach_ledger(engine, ledger);

    /* ---- кошелёк: live — истина с цепи (balanceOf Safe), иначе 5 WETH -- */
    tb_wallet_t *wallet = (tb_wallet_t *)tb_exec_wallet(engine);
    tb_u256_t start = tb_u256_from_u64(5000000000000000000ull);
    if (live_exec) {
        tb_error_t berr = TB_ERROR_INIT;
        if (tb_safe_balance_of(http, &cfg.dex.safe, &cfg.weth,
                               &cfg.dex.safe.safe, &start, &berr) != 0) {
            fprintf(stderr, "balanceOf(Safe): %s\n", berr.message);
            return 1;
        }
        if (tb_u256_is_zero(&start)) {
            fprintf(stderr,
                    "live: на Safe нет WETH — пополните контракт\n");
            return 1;
        }
    }
    tb_wallet_deposit(wallet, &cfg.weth, &start, &err);

    /* ---- цикл + мост BLOCKED->таймер ---- */
    app.loop = tb_loop_create(2, &err);
    if (!app.loop) {
        fprintf(stderr, "цикл: %s\n", err.message);
        return 1;
    }
    g_app.loop = app.loop; /* теперь обработчик сигналов может остановить цикл */
    tb_evreg_on_state(app.reg, 1u << TB_EV_BLOCKED, bridge_observer, &app);

    /* ---- конвейер арбитража ---- */
    tb_arb_t *arb = tb_arb_create(&cfg, engine, ledger, http, &err);
    if (!arb) {
        fprintf(stderr, "arb: %s\n", err.message);
        return 1;
    }
    if (tb_arb_start(arb, app.loop, &err) != 0) {
        fprintf(stderr, "arb start: %s\n", err.message);
        return 1;
    }

    /* ---- веб-морда: дашборд + статус + журнал (127.0.0.1) ---- */
    app.arb = arb;
    app.engine = engine;
    app.cfg = &cfg;
    app.start_weth = start;
    app.started_ms = tb_now_ms();
    if (web_port > 0) {
        tb_error_t werr = TB_ERROR_INIT;
        if (tb_webui_start(&app.web, (uint16_t)web_port, ledger_path,
                           &werr) != 0) {
            fprintf(stderr, "веб-морда: %s (продолжаем без неё)\n",
                    werr.message);
        } else {
            printf("веб-морда: http://127.0.0.1:%u\n",
                   (unsigned)tb_webui_port(app.web));
            tb_loop_call_every(app.loop, 2000, TB_PRIO_LOW, "web.update",
                               web_update_task, &app, NULL);
            web_update_task(app.loop, NULL, &app);
        }
    }

    {
        char mn[40], mx[40];
        tb_u256_t v = tb_u256_from_u64(cfg.min_trade_wei);
        eth_str(&v, mn, sizeof(mn));
        v = tb_u256_from_u64(cfg.max_trade_wei);
        eth_str(&v, mx, sizeof(mx));
        printf("арбитраж: %u пулов (%u бирж), период %llu мс, минимум %s ETH, "
               "верх контракта %s ETH\n",
               cfg.pools_n, cfg.dex.venues_n,
               (unsigned long long)cfg.poll_ms, mn, mx);
    }

    /* ---- прогон ---- */
    if (duration_ms > 0)
        tb_loop_call_later(app.loop, duration_ms, TB_PRIO_CRITICAL, "stop",
                            stop_task, NULL, NULL);
    tb_loop_run(app.loop);
    g_app.loop = NULL; /* цикл остановлен: сигналы больше его не трогают */

    /* веб-морду глушим до разрушения объектов, которые она показывала */
    if (app.web) {
        tb_webui_stop(app.web);
        app.web = NULL;
    }

    /* ---- итоги ---- */
    const tb_arb_stats_t *as = tb_arb_stats(arb);
    const tb_exec_stats_t *es = tb_exec_stats(engine);
    const tb_exec_dex_stats_t *ds = tb_exec_dex_stats(&dexb_exec);
    const tb_exec_dex_stats_t *dq = tb_arb_quote_stats(arb); /* реальный
        цитировщик: свои eth_call к QuoterV2 (у арбитража свой экземпляр) */

    tb_u256_t bal = tb_wallet_balance(wallet, &cfg.weth);
    tb_u256_t pnl;
    bool have = tb_u256_sub(&pnl, &bal, &start);
    char pnl_s[80], bal_s[80], start_s[80];
    tb_u256_t zero = tb_u256_from_u64(0);
    eth_str(have ? &pnl : &zero, pnl_s, sizeof(pnl_s));
    eth_str(&bal, bal_s, sizeof(bal_s));
    eth_str(&start, start_s, sizeof(start_s));

    printf("\n================= ИТОГИ =================\n");
    printf("Опросы: %llu | котировок собрано: %llu | отсеяно фильтрами: "
           "%llu\n",
           (unsigned long long)as->polls,
           (unsigned long long)as->quotes,
           (unsigned long long)as->filter_rejects);
    printf("Пар оценено: %llu | выгодных: %llu | ошибок котировщика: %llu\n",
           (unsigned long long)as->pairs_evaluated,
           (unsigned long long)as->pairs_profitable,
           (unsigned long long)as->quote_errors);
    printf("Ордеров подано: %llu\n",
           (unsigned long long)as->orders_submitted);
    printf("Сделки: filled=%llu failed=%llu canceled=%llu (невыгодность)\n",
           (unsigned long long)es->filled,
           (unsigned long long)es->failed,
           (unsigned long long)es->canceled);
    printf("Отмены: %llu, потерянный газ: %llu wei\n",
           (unsigned long long)as->orders_canceled,
           (unsigned long long)as->cancel_fee_wei);
    printf("RPC: quote-вызовы %llu, exec-вызовы %llu\n",
           (unsigned long long)dq->rpc_calls,
           (unsigned long long)ds->rpc_calls);
    printf("Кошелёк WETH: %s (старт %s)\n", bal_s, start_s);
    printf(have ? "Виртуальный PnL: %s ETH\n"
                : "Виртуальный PnL: −%s ETH (баланс ниже старта)\n",
           pnl_s);
    printf("Журнал: %s\n", ledger_path);

    /* Порядок разборки: сначала цикл — join воркеров, иначе цитировщик
     * ещё в блокирующем RPC, а бэкенды под ним уже освобождены
     * (краш в venue_by_pool). Дальше — то, чем пользовались воркеры. */
    tb_loop_destroy(app.loop);
    tb_arb_destroy(arb);
    tb_exec_destroy(engine);
    tb_ledger_close(ledger);
    tb_http_destroy(http);
    if (replay)
        mock_stop(&app.mock);
    return 0;
}
