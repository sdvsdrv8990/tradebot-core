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
static uint64_t mock_quote(const char *data_hex); /* V3 */

/* Общая математика хопа: направления, тир-модификаторы, влияние размера
 * (покупка WETH->TOKEN / продажа TOKEN->WETH). */
static uint64_t mock_hop(const char *in_tok, const char *out_tok,
                         uint64_t amt, uint32_t fee)
{
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

/* Разбор calldata QuoterV2 -> mock_hop. */
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

    return mock_hop(in_tok, out_tok, amt, fee);
}

/* Разбор calldata Router.getAmountsOut(uint256, address[]) -> выход пути.
 * Комиссия V2 фиксирована 0.30% — тир-модификатор 3000. */
static uint64_t mock_amounts_out(const char *data_hex)
{
    /* 0x + selector(8) + amount(64) + offset(64) + len(64) + tokA + tokB */
    const char *w_amt = data_hex + 10;
    const char *w_a = data_hex + 10 + 128;
    const char *w_b = data_hex + 10 + 192;

    char num[32];
    memcpy(num, w_amt, 16);
    num[16] = '\0';
    if (strtoull(num, NULL, 16))
        return 0;
    memcpy(num, w_amt + 48, 16);
    num[16] = '\0';
    uint64_t amt = strtoull(num, NULL, 16);

    char in_tok[41], out_tok[41];
    memcpy(in_tok, w_a + 24, 40);
    in_tok[40] = 0;
    memcpy(out_tok, w_b + 24, 40);
    out_tok[40] = 0;
    return mock_hop(in_tok, out_tok, amt, 3000);
}

/* Ответ eth_call symbol()/decimals(): ABI-данные по адресу токена.
 * Слово-выровненный ответ; symbol — строка ABI, decimals — слово. */
static int mock_token_call(const char *to_hex, const char *data_hex,
                           char *result /* до четырёх ABI-слов */)
{
    static const struct {
        const char *hex, *sym;
        uint32_t dec;
    } toks[] = {
        { WETH_HEX, "WETH", 18 },
        { USDC_HEX, "USDC", 6 },
        { USDT_HEX, "USDT", 6 },
        { "6b175474e89094c44da98b954eedeac495271d0f", "DAI", 18 },
        { "2260fac5e5542a773aa44fbcfedf7c193bc2c599", "WBTC", 8 },
        { "514910771af9ca656af840dff83e8264ecf986ca", "LINK", 18 },
        { "1f9840a85d5af5bf1d1762f925bdaddc4201f984", "UNI", 18 },
        { "95ad61b0a150d79219dcf64e1e6cc01f0b64c4ce", "SHIB", 18 },
        { "6982508145454ce325ddbe47a25d4ec3d2311933", "PEPE", 18 },
        { "7fc66500c84a76ad7e9c93437bfc5ac33e2ddae9", "AAVE", 18 },
        { "5a98fcbea516cf06857215779fd812ca3bef1b32", "LDO", 18 },
    };
    uint32_t dec = 0;
    const char *sym = NULL;
    for (size_t i = 0; i < sizeof(toks) / sizeof(toks[0]); i++)
        if (strncasecmp(to_hex, toks[i].hex, 40) == 0) {
            sym = toks[i].sym;
            dec = toks[i].dec;
            break;
        }
    if (!sym)
        return 0;

    if (strncasecmp(data_hex, "0x95d89b41", 10) == 0) { /* symbol() */
        /* offset(0x20) + len + данные, выровненные в слово */
        size_t slen = strlen(sym);
        memset(result + 2, '0', 128);
        result[2 + 62] = '2'; /* слово0 = 0x20 */
        result[2 + 63] = '0';
        for (size_t i = 0; i < 16; i++) {
            int d = (int)(((slen >> (60 - 4 * i)) & 0xf));
            result[2 + 64 + 63 - i] = "0123456789abcdef"[d];
        }
        for (size_t i = 0; i < slen; i++) { /* байты строки (hex-пара) */
            result[2 + 128 + i * 2] =
                "0123456789abcdef"[(sym[i] >> 4) & 0xf];
            result[2 + 129 + i * 2] = "0123456789abcdef"[sym[i] & 0xf];
        }
        result[2 + 192] = '\0'; /* три слова: дальше буфер для V2-массива */
        return 1;
    }
    if (strncasecmp(data_hex, "0x313ce567", 10) == 0) { /* decimals() */
        memset(result + 2, '0', 64);
        for (int i = 0; i < 8; i++) /* 8 nibble: значение 32-битное,
                                     * сдвиг на 32+ — UB и мусор в слове */
            result[2 + 64 - 1 - i] =
                "0123456789abcdef"[(dec >> (4 * i)) & 0xf];
        result[2 + 64] = '\0'; /* одно слово, не 3-словный буфер */
        return 1;
    }
    return 0;
}

/* hex-символ -> значение; 0 для не-hex (в mock-запросах мусора не бывает:
 * calldata собирает бот). */
static unsigned hexv(char c)
{
    if (c >= '0' && c <= '9')
        return (unsigned)(c - '0');
    if (c >= 'a' && c <= 'f')
        return (unsigned)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F')
        return (unsigned)(c - 'A' + 10);
    return 0;
}

/* Значение -> ABI-слово (64 hex-символа, выровнено вправо). */
static void mock_word_hex(char *dst64, uint64_t v)
{
    memset(dst64, '0', 48);
    for (int i = 0; i < 16; i++)
        dst64[48 + i] = "0123456789abcdef"[(v >> (4 * (15 - i))) & 0xf];
}

static void *mock_thread(void *arg)
{
    mock_rpc_t *m = arg;
    uint64_t block = 21000000;
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

        char result[300] = "0x"; /* до четырёх ABI-слов (массив V2) */
        memset(result + 2, '0', 256);
        result[258] = '\0';
        uint64_t out = 0;
        int ok = 0;

        if (strstr(body, "\"eth_blockNumber\"")) { /* В6: блок батча */
            block++;
            for (int i = 15; i >= 0; i--) {
                result[2 + i] = "0123456789abcdef"[block >> (4 * (15 - i)) & 0xf];
            }
            result[18] = '\0'; /* компактный hex достаточно */
            ok = 1;
        } else {
            const char *to = strstr(body, "\"to\":\"0x");
            const char *d = strstr(body, "\"data\":\"0x");
            if (getenv("TB_MOCK_DEBUG") && to && d)
                fprintf(stderr, "[mock] to=%.48s data=%.16s\n", to, d);
            if (to && d)
                ok = mock_token_call(to + 8, d + 8, result);
            if (!ok && d
                && strncasecmp(d + 8, "0xd06ca61f", 10) == 0) {
                /* V2: Router.getAmountsOut -> uint[2] [вход, выход] */
                uint64_t amt = 0;
                const char *w_amt = d + 8 + 10;
                for (int i = 48; i < 64; i++)
                    amt = amt * 16 + (uint64_t)hexv(w_amt[i]);
                out = mock_amounts_out(d + 8);
                ok = 1;
                result[2 + 63] = '2'; /* слово0: смещение 0x20 */
                result[2 + 64 + 63] = '2'; /* слово1: длина 2 */
                mock_word_hex(result + 2 + 128, amt);  /* вход */
                mock_word_hex(result + 2 + 192, out);  /* выход */
                result[258] = '\0';
            }
            if (!ok && d) {
                out = mock_quote(d + 8);
                ok = 1;
                for (int i = 0; i < 16; i++) { /* слово 0: значение
                                                 * выровнено ВПРАВО (ABI) */
                    int digit = (int)((out >> ((15 - i) * 4)) & 0xf);
                    result[2 + 48 + i] = "0123456789abcdef"[digit];
                }
            }
        }
        char resp[512];
        int n;
        if (getenv("TB_MOCK_DEBUG") && ok)
            fprintf(stderr, "[mock] resp len=%zu result=[%s]\n", strlen(result), result);
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
    if (g_app.arb)
        tb_arb_request_stop(g_app.arb); /* атомарно: воркеры сворачиваются
                                         * между RPC — цикл не ждёт
                                         * доработки очереди */
    if (g_app.loop)
        tb_loop_stop(g_app.loop); /* как в bot_demo: из обработчика можно */
}

static void stop_task(tb_loop_t *loop, tb_task_t *task, void *arg)
{
    (void)task;
    (void)arg;
    if (g_app.arb)
        tb_arb_request_stop(g_app.arb);
    tb_loop_stop(loop);
}

/* ---- Страницы-микромодули (JS фетчит свой api и рисует в #page;
 * оболочка/навигация/автообновление — в webui). Список поднятых
 * страниц управляется конфигом: web.pages. ---- */

#define WEBUI_MARKET_JSON_MAX (16 * 1024)

static const char DASH_JS[] =
    /* Каркас один раз; карточки патчатся по-ячейково, таблица журнала —
     * rowsSync и только когда записи реально сместились. */
    "let dbuilt=false;"
    "const dbuild=()=>{if(dbuilt)return;"
    "document.getElementById('page').innerHTML="
    "'<div class=\"cards\" id=\"cards\"></div>'"
    "+'<h2>Последние решения и сделки</h2>'"
    "+'<table><thead><tr><th>время</th><th>тип</th><th>детали</th></tr>"
    "</thead><tbody id=\"dt\"></tbody></table>';"
    "dbuilt=true};"
    "const dashRow=r=>{let d='';const t=r.type||'',p=r.payload||{};"
    "if(t.indexOf('decision.evaluated')===0){"
    "const ps=p.pairs||[];d='пар '+ps.length+'; выгодных '+"
    "ps.filter(x=>x.ok).length;}"
    "else if(t.indexOf('decision.submitted')===0){"
    "d='event '+(p.event_id||'?')+' · размер '+String(p.size||'')+' wei';}"
    "else if(t.indexOf('decision.canceled')===0){d=p.reason||'';}"
    "else if(t.indexOf('trade.')===0){"
    "const dt=p.data||{};"
    "d=(dt.amount_in?('вход '+String(dt.amount_in).slice(0,10)+'… · '):'')"
    "+(p.operation||'')+((p.error&&p.error.message)?(' · '+p.error.message):'');}"
    "const cls=t.indexOf('trade.executed')===0?'t-ok':"
    "(t.indexOf('trade.failed')===0?'t-err':"
    "(t.indexOf('trade.submitted')===0?'t-pend':''));"
    "return [fmt(r.ts_ms||0),"
    "cls?('<span class=\"tag '+cls+'\">'+esc(t)+'</span>'):esc(t),esc(d)]};"
    "async function tick(){try{"
    "const st=await (await fetch('/api/status')).json();"
    "dbuild();"
    "document.getElementById('mode').textContent="
    "esc(st.mode||'')+' · аптайм '+Math.round((st.uptime_ms||0)/1000)+'с';"
    "const cards=st.cards||[];const box=document.getElementById('cards');"
    "if(box.children.length!==cards.length)"
    "box.innerHTML=cards.map(k=>'<div class=\"card\"><b></b><span></span></div>').join('');"
    "cards.forEach((k,i)=>{const c=box.children[i];"
    "const cls='card'+(k.cls?' '+k.cls:'');"
    "if(c.className!==cls)c.className=cls;"
    "cellSync(c.children[0],esc(k.v));"
    "cellSync(c.children[1],esc(k.t))});"
    "try{"
    "const lg=await (await fetch('/api/ledger')).json();"
    "const recs=lg.records||[];"
    "const lk=recs.length+':'+(recs[0]?(recs[0].ts_ms||''):'');"
    "if(changed('dash.lg',lk))"
    "rowsSync(document.getElementById('dt'),recs.slice(-40).reverse().map(dashRow));"
    "}catch(e){}"
    "}catch(e){}}";

static const char VENUES_JS[] =
    /* Каркас строится ОДИН раз; дальше — только точечные патчи ячеек
     * (cellSync/rowsSync из оболочки): цены меняются без пересборки
     * таблицы, скролл и логотипы не дёргаются. */
    "let vbuilt=false;"
    "const vbuild=()=>{if(vbuilt)return;"
    "document.getElementById('page').innerHTML="
    "'<div class=\"cards\" id=\"qcards\"></div>'"
    "+'<h2>Пары по выгодности (сверху лучшие)</h2>"
    "<table><thead><tr><th>токен</th><th>покупка</th><th>продажа</th>"
    "<th>спред</th><th>≈ спред-оценка ETH</th></tr></thead>"
    "<tbody id=\"pt\"></tbody></table>'"
    "+'<h2>Пулы под наблюдением</h2>"
    "<table><thead><tr><th>биржа</th><th>пул</th><th>fee</th>"
    "<th>TOKEN за пробу</th><th>круг туда-обратно</th><th>блок</th>"
    "<th>свежесть</th></tr></thead><tbody id=\"pt2\"></tbody></table>';"
    "vbuilt=true};"
    "async function tick(){try{"
    "const m=await (await fetch('/api/market')).json();"
    "const key=JSON.stringify(m).replace(/\"(ts_ms|uptime_ms)\":\d+/g,'');"
    "if(!changed('venues',key))return;"
    "vbuild();"
    "document.getElementById('mode').textContent='биржи и цены · '+"
    "((m.venues||[]).length)+' бирж · проба '+eth(m.probe_wei)+' ETH';"
    "const q=m.quote;"
    "cellSync(document.getElementById('qcards'),"
    "q?('<div class=\"card'+(String(q.profit_wei||'').startsWith('-')?' bad':' good')+'\">"
    "<b>'+eth(String(q.profit_wei||'').replace(/^-/,''))+'</b>"
    "<span>реальная котировка '+esc(q.symbol||'')+' · '+esc(q.buy)"
    "+' → '+esc(q.sell)+' · вход '+eth(q.in_wei)+' ETH</span></div>'"
    "+'<div class=\"card\"><b>'+bp(q.spread_bp)+'</b>"
    "<span>оценка спреда той же пары</span></div>'):'');"
    "let pairs=(m.pairs||[]).map(p=>["
    "tokLogo(p.token)"
    "+'<a class=\"dex\" title=\"'+p.token+'\" "
    "href=\"https://etherscan.io/token/'+p.token"
    "+'\" target=\"_blank\" rel=\"noreferrer\">'+esc(p.symbol||"
    "short(p.token))+'</a>',"
    "dexCell(p.buy),dexCell(p.sell),"
    "'<span style=\"color:'+(p.spread_bp>0?'#5dd39e':'#7b8494')+'\">'"
    "+bp(p.spread_bp)+'</span>',"
    "eth(p.profit_est)]);"
    "if(!pairs.length)"
    "pairs=[['<td colspan=\"5\" class=\"mut\">нет свежих пар с положительным "
    "спредом</td>']];"
    "rowsSync(document.getElementById('pt'),pairs);"
    "const pools=(m.venues||[]).flatMap(v=>(v.pools||[]).map(pl=>["
    "dexCell(v.name),"
    "'<span title=\"'+esc(pl.pool)+'\">'+short(pl.pool)+'</span>',"
    "esc(pl.fee),short(pl.buy_out),bp(pl.round_bp-10000),"
    "String(pl.block||'—'),"
    "pl.fresh?'свежая':'<span class=\"tag t-err\">устарела</span>']));"
    "rowsSync(document.getElementById('pt2'),pools);"
    "}catch(e){/* каркас жив: данные придёт со следующим тиком */}}";

static const char TRADES_JS[] =
    "let tbuilt=false;"
    "const tbuild=()=>{if(tbuilt)return;"
    "document.getElementById('page').innerHTML="
    "'<table><thead><tr><th>время</th><th>статус</th><th>маршрут</th>"
    "<th>вход ETH</th><th>выход ETH</th><th>профит ETH</th><th>ордер</th>"
    "<th>ошибка</th></tr></thead><tbody id=\"tt\"></tbody></table>';"
    "tbuilt=true};"
    "const tradeRow=r=>{const t=r.type||'',p=r.payload||{},dt=p.data||{};"
    "let tag='',st=t;"
    "if(t.indexOf('trade.executed')===0){tag='t-ok';st='filled'}"
    "else if(t.indexOf('trade.failed')===0){tag='t-err';st='failed'}"
    "else if(t.indexOf('trade.canceled')===0){tag='t-err';st='canceled'}"
    "else if(t.indexOf('trade.submitted')===0){tag='t-pend';st='pending'}"
    "let prof='';"
    "if(dt.amount_in&&dt.amount_out){const d=big(dt.amount_out)"
    "-big(dt.amount_in);prof=(d>=0n?'+':'')+eth(d.toString())}"
    "const errm=(p.error&&p.error.message)?esc(p.error.message):'';"
    "return [fmt(r.ts_ms||0),"
    "tag?('<span class=\"tag '+tag+'\">'+st+'</span>'):st,"
    "short(dt.token_in),eth(dt.amount_in),eth(dt.amount_out),prof,"
    "short(String(dt.order||'')),errm]};"
    "async function tick(){try{"
    "const lg=await (await fetch('/api/trades')).json();"
    "const recs=lg.records||[];"
    "tbuild();"
    "document.getElementById('mode').textContent='сделки · всего '"
    "+recs.length;"
    "const k=recs.length+':'+(recs[0]?(recs[0].ts_ms||''):'');"
    "if(changed('trades',k))"
    "rowsSync(document.getElementById('tt'),recs.slice(-100).reverse().map(tradeRow));"
    "}catch(e){}}";

static const tb_web_page_t WEB_PAGES[] = {
    {"dashboard", "/", "Дашборд", DASH_JS},
    {"venues", "/venues", "Биржи и цены", VENUES_JS},
    {"trades", "/trades", "Сделки", TRADES_JS},
};

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
    tb_webui_set_named(d->web, "status", json);

    /* рынок для страницы «Биржи и цены» (тот же источник: tb_arb;
     * сборка — чистая функция в tb_user, ворота свежести/блока — там) */
    char market[WEBUI_MARKET_JSON_MAX];
    size_t snaps_n = 0;
    const tb_arb_snap_t *snaps = tb_arb_last_snaps(d->arb, &snaps_n);
    tb_arb_market_json(d->cfg, snaps, snaps_n, tb_now_ms(),
                       tb_arb_route_quote(d->arb), market, sizeof(market));
    tb_webui_set_named(d->web, "market", market);
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
    long web_port = 0; /* 0 = не задан (возьмём из конфига), -1 = --no-web */

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
            web_port = -1;
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
    tb_arb_defaults(&cfg); /* в т.ч. web-страницы/порт/период */

    /* В2: целостность конфига — до сети и до движка (fail-fast) */
    if (tb_arb_cfg_validate(&cfg, &err) != 0) {
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
        /* офлайн: локальный mock-RPC + демо-параметры экономии.
         * ВСЕ venue на mock: рынок синтетический, включая V2
         * (getAmountsOut) — межбиржевые маршруты гоняются офлайн. */
        mock_start(&app.mock);
        for (uint32_t v = 0; v < cfg.dex.venues_n; v++)
            snprintf(cfg.dex.venues[v].rpc_url,
                     sizeof(cfg.dex.venues[v].rpc_url),
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

    /* ---- веб-морда: поднимаем ДО медленных стартерных eth_call —
     * страница отвечает сразу («котировок ещё нет»), данные подтянутся
     * первым решённым батчем; bind из конфига (web.bind) ---- */
    if (web_port == 0)
        web_port = cfg.web.port; /* не задано флагом — берём из конфига */
    if (web_port > 0) {
        tb_error_t werr = TB_ERROR_INIT;
        if (tb_webui_start(&app.web, (uint16_t)web_port, cfg.web.bind,
                           ledger_path, &werr) != 0) {
            fprintf(stderr, "веб-морда: %s (продолжаем без неё)\n",
                    werr.message);
        } else {
            for (uint32_t k = 0; k < cfg.web.pages_n; k++) {
                bool known = false;
                for (size_t q = 0;
                     q < sizeof(WEB_PAGES) / sizeof(WEB_PAGES[0]); q++)
                    if (strcmp(WEB_PAGES[q].id, cfg.web.pages[k]) == 0) {
                        tb_webui_page_add(app.web, &WEB_PAGES[q]);
                        known = true;
                        break;
                    }
                if (!known)
                    fprintf(stderr, "веб-морда: неизвестная страница '%s'\n",
                            cfg.web.pages[k]);
            }
            printf("веб-морда: http://%s:%u%s\n", cfg.web.bind,
                   (unsigned)tb_webui_port(app.web),
                   strcmp(cfg.web.bind, "127.0.0.1") == 0
                       ? ""
                       : " (доступна с других машин)");
            fflush(stdout);
        }
    }

    /* В4: идентичность пулов на цепи — каждый пул обязан совпасть с
     * Factory.getPool/getPair своей биржи (replay — синтетика, мимо) */
    if (!replay) {
        printf("старт: проверка %u пулов на цепи (getPool/getPair)…\n",
               cfg.pools_n);
        fflush(stdout);
        if (tb_arb_cfg_verify(&cfg, http, &err) != 0) {
            fprintf(stderr, "проверка на цепи: %s\n", err.message);
            if (app.web)
                tb_webui_stop(app.web);
            tb_http_destroy(http);
            return 1;
        }
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
    for (uint32_t i = 0; i < cfg.pools_n; i++) {
        bool dup = false;
        for (uint32_t k = 0; k < ecfg.allow_n; k++)
            if (tb_addr_eq(&ecfg.allow[k], &cfg.pools[i].token))
                dup = true;
        if (!dup)
            ecfg.allow[1 + ecfg.allow_n++] = cfg.pools[i].token;
        /* мультичейн: хаб каждой цепи тоже в allow (ордера идут в нём) */
        dup = false;
        for (uint32_t k = 0; k < ecfg.allow_n; k++)
            if (tb_addr_eq(&ecfg.allow[k], &cfg.pools[i].weth))
                dup = true;
        if (!dup && ecfg.allow_n < TB_EXEC_ALLOW_MAX - 1)
            ecfg.allow[1 + ecfg.allow_n++] = cfg.pools[i].weth;
    }
    ecfg.allow_n++; /* +хаб */
    if (ecfg.allow_n >= TB_EXEC_ALLOW_MAX - 1 && cfg.pools_n > 0) {
        /* fail-fast: молчаливое обрезание allow отклоняло бы ордера
         * по неочевидной причине */
        bool dropped = false;
        for (uint32_t i = 0; i < cfg.pools_n && !dropped; i++) {
            bool ok = tb_addr_eq(&cfg.pools[i].token, &cfg.weth);
            for (uint32_t k = 0; k < ecfg.allow_n && !ok; k++)
                ok = tb_addr_eq(&ecfg.allow[k], &cfg.pools[i].token)
                     || tb_addr_eq(&ecfg.allow[k], &cfg.pools[i].weth);
            dropped = !ok;
        }
        if (dropped) {
            fprintf(stderr,
                    "allow-лист движка (%d) не вместил все хабы/токены "
                    "конфига — уменьшите пулы\n",
                    TB_EXEC_ALLOW_MAX);
            return 1;
        }
    }
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
    /* 6 воркеров: батч 30 пулов × 3 котировки (две цепи), каждая — своё
     * TLS-рукопожатие (транспорт без keep-alive); 4 воркера тянули
     * батч ~35 с — впритык к свежести */
    app.loop = tb_loop_create(6, &err);
    if (!app.loop) {
        fprintf(stderr, "цикл: %s\n", err.message);
        return 1;
    }
    g_app.loop = app.loop; /* теперь обработчик сигналов может остановить цикл */
    tb_evreg_on_state(app.reg, 1u << TB_EV_BLOCKED, bridge_observer, &app);

    /* ---- конвейер арбитража ---- */
    if (!replay) {
        printf("старт: symbol/decimals токенов с цепи…\n");
        fflush(stdout);
    }
    tb_arb_t *arb = tb_arb_create(&cfg, engine, ledger, http, &err);
    if (!arb) {
        fprintf(stderr, "arb: %s\n", err.message);
        return 1;
    }
    if (tb_arb_start(arb, app.loop, &err) != 0) {
        fprintf(stderr, "arb start: %s\n", err.message);
        return 1;
    }

    /* ---- обновление страниц морды: конвейер жив, данные пойдут ---- */
    app.arb = arb;
    app.engine = engine;
    app.cfg = &cfg;
    app.start_weth = start;
    app.started_ms = tb_now_ms();
    if (app.web) {
        if (!cfg.web.refresh_ms)
            cfg.web.refresh_ms = 2000;
        tb_loop_call_every(app.loop, cfg.web.refresh_ms, TB_PRIO_LOW,
                           "web.update", web_update_task, &app, NULL);
        web_update_task(app.loop, NULL, &app);
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
    /* статистика цитировщика пишется воркерами атомарно — читаем так же:
     * цикл уже остановлен, но последний воркер ещё может довести RPC */
    printf("RPC: quote-вызовы %llu, exec-вызовы %llu\n",
           (unsigned long long)tb_atomic_load_u64(&dq->rpc_calls),
           (unsigned long long)tb_atomic_load_u64(&ds->rpc_calls));
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
    tb_exec_destroy(engine); /* заимствованный dex-бэкенд — наш */
    if (dexb_exec.destroy)
        dexb_exec.destroy(&dexb_exec);
    tb_ledger_close(ledger);
    tb_http_destroy(http);
    if (replay)
        mock_stop(&app.mock);
    return 0;
}
