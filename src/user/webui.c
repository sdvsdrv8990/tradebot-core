#define TB_CURRENT_GROUP TB_G_ARB /* зона ответственности файла */

/* ==========================================================================
 *  tb/user/webui.c — веб-морда: дашборд + JSON API на 127.0.0.1.
 *
 *  Гонок нет по построению: поток сервера читает только (а) снапшот
 *  статуса под мьютексом, (б) файл журнала собственным read-only fd
 *  (append-only JSONL; частичная последняя строка отбрасывается).
 *  К памяти приложения сервер не прикасается.
 * ========================================================================== */
#include "tb/user/webui.h"
#include "tb/arch/arch.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define WEBUI_SNAPSHOT_MAX 4096
#define WEBUI_LEDGER_TAIL  (64 * 1024)

typedef struct tb_webui {
    int          fd;
    uint16_t     port;
    pthread_t    th;
    volatile bool stop;
    pthread_mutex_t lock;
    char         snapshot[WEBUI_SNAPSHOT_MAX]; /* JSON статуса */
    char         ledger_path[512];
} tb_webui_t;

/* ---- дашборд (одна страница, без зависимостей) ---- */

static const char PAGE[] =
    "<!doctype html><html lang=\"ru\"><head><meta charset=\"utf-8\">"
    "<title>tradebot-core</title><style>"
    ":root{color-scheme:dark}body{font:14px/1.45 system-ui,sans-serif;"
    "background:#111318;color:#d7dce3;margin:24px}"
    "h1{font-size:18px;margin:0 0 4px}.sub{color:#7b8494;font-size:12px}"
    ".cards{display:flex;flex-wrap:wrap;gap:10px;margin:16px 0}"
    ".card{background:#1a1e26;border:1px solid #262c37;border-radius:8px;"
    "padding:10px 14px;min-width:110px}"
    ".card b{display:block;font-size:20px;color:#e8ecf1}"
    ".card span{font-size:11px;color:#7b8494}"
    ".good b{color:#5dd39e}.bad b{color:#e07a5f}"
    "table{border-collapse:collapse;width:100%;font-size:12.5px}"
    "th,td{padding:5px 8px;border-bottom:1px solid #232935;text-align:left;"
    "white-space:nowrap;overflow:hidden;text-overflow:ellipsis;max-width:"
    "340px}th{color:#7b8494;font-weight:500}"
    "h2{font-size:13px;color:#9aa4b2;margin:18px 0 6px;font-weight:600}"
    ".tag{display:inline-block;padding:1px 7px;border-radius:10px;"
    "font-size:11px}.t-ok{background:#1d3a2f;color:#5dd39e}"
    ".t-err{background:#3a241f;color:#e07a5f}.t-pend{background:#33301d;"
    "color:#d9b45a}"
    "</style></head><body>"
    "<h1>tradebot-core</h1><div class=\"sub\" id=\"mode\">…</div>"
    "<div class=\"cards\" id=\"cards\"></div>"
    "<h2>Последние решения и сделки</h2>"
    "<table><thead><tr><th>время</th><th>тип</th><th>детали</th>"
    "</tr></thead><tbody id=\"rows\"></tbody></table>"
    "<script>"
    "const esc=s=>String(s==null?'':s).replace(/[&<>]/g,c=>({'&':'&amp;',"
    "'<':'&lt;','>':'&gt;'}[c]));"
    "function fmt(t){return new Date(t).toLocaleTimeString('ru-RU')}"
    "async function tick(){"
    "try{"
    "const st=await (await fetch('/api/status')).json();"
    "document.getElementById('mode').textContent="
    "esc(st.mode||'')+' · аптайм '+Math.round((st.uptime_ms||0)/1000)+'с';"
    "const c=document.getElementById('cards');"
    "c.innerHTML=(st.cards||[]).map(k=>'<div class=\"card'+(k.cls?' '+k.cls:"
    "'')+'\"><b>'+esc(k.v)+'</b><span>'+esc(k.t)+'</span></div>').join('');"
    "}catch(e){}"
    "try{"
    "const lg=await (await fetch('/api/ledger')).json();"
    "const rows=(lg.records||[]).slice(-40).reverse().map(r=>{"
    "let d='';const t=r.type||'',p=r.payload||{};"
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
    "return '<tr><td>'+fmt(r.ts_ms||0)+'</td><td>'+(cls?('<span "
    "class=\"tag '+cls+'\">'):'')+esc(t)+(cls?'</span>':'')+'</td><td>'"
    "+esc(d)+'</td></tr>';}).join('');"
"document.getElementById('rows').innerHTML=rows;"
    "}catch(e){}}"
    "tick();setInterval(tick,2000);"
    "</script></body></html>";

/* ---- HTTP-ответы ---- */

static void send_all(int c, const char *p, size_t n)
{
    while (n) {
        ssize_t w = send(c, p, n, MSG_NOSIGNAL);
        if (w <= 0)
            return;
        p += w;
        n -= (size_t)w;
    }
}

static void respond(int c, const char *ctype, const char *body,
                    size_t body_len)
{
    char head[160];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 200 OK\r\nContent-Type: %s\r\n"
                     "Content-Length: %zu\r\nCache-Control: no-store\r\n"
                     "Connection: close\r\n\r\n",
                     ctype, body_len);
    if (n > 0)
        send_all(c, head, (size_t)n);
    send_all(c, body, body_len);
}

static void respond_status_json(tb_webui_t *w, int c)
{
    char snap[WEBUI_SNAPSHOT_MAX];
    pthread_mutex_lock(&w->lock);
    memcpy(snap, w->snapshot, sizeof(snap));
    pthread_mutex_unlock(&w->lock);
    if (!snap[0])
        strcpy(snap, "{\"cards\":[],\"mode\":\"запуск…\"}");
    respond(c, "application/json", snap, strlen(snap));
}

/* Хвост журнала: последние строки JSONL как массив объектов. */
static void respond_ledger(tb_webui_t *w, int c)
{
    FILE *f = fopen(w->ledger_path, "rb");
    if (!f) {
        respond(c, "application/json", "{\"records\":[]}", 15);
        return;
    }
    static char buf[WEBUI_LEDGER_TAIL + 1];
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    long back = size > WEBUI_LEDGER_TAIL ? WEBUI_LEDGER_TAIL : size;
    fseek(f, -back, SEEK_END);
    size_t n = fread(buf, 1, (size_t)back, f);
    fclose(f);
    buf[n] = '\0';

    /* строка с первого \n (частичную голову отбрасываем) */
    char *start = n == (size_t)size ? buf : strchr(buf, '\n');
    start = start ? start + 1 : buf;

    static char out[WEBUI_LEDGER_TAIL + 64];
    size_t o = snprintf(out, sizeof(out), "{\"records\":[");
    char *save = NULL;
    int first = 1;
    for (char *line = strtok_r(start, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        if (line[0] != '{')
            continue; /* частичный хвост или мусор */
        size_t ll = strlen(line);
        while (ll && (line[ll - 1] == '\r'))
            line[--ll] = '\0';
        if (ll < 2 || line[ll - 1] != '}')
            continue; /* оборванная строка */
        if (o + ll + 4 >= sizeof(out))
            break;
        o += (size_t)snprintf(out + o, sizeof(out) - o, "%s%s", first ? "" : ",",
                              line);
        first = 0;
    }
    snprintf(out + o, sizeof(out) - o, "]}");
    respond(c, "application/json", out, strlen(out));
}

static void serve_client(tb_webui_t *w, int c)
{
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
    if (strncmp(req, "GET /api/status", 15) == 0)
        respond_status_json(w, c);
    else if (strncmp(req, "GET /api/ledger", 15) == 0)
        respond_ledger(w, c);
    else if (strncmp(req, "GET / ", 6) == 0
             || strncmp(req, "GET /index.html", 15) == 0)
        respond(c, "text/html; charset=utf-8", PAGE, sizeof(PAGE) - 1);
    else {
        static const char nf[] = "HTTP/1.1 404 Not Found\r\n"
                                 "Content-Length: 0\r\nConnection: close"
                                 "\r\n\r\n";
        send_all(c, nf, sizeof(nf) - 1);
    }
}

static void *web_thread(void *arg)
{
    tb_webui_t *w = arg;
    while (!tb_atomic_load_bool(&w->stop)) {
        int c = accept(w->fd, NULL, NULL);
        if (c < 0) {
            if (tb_atomic_load_bool(&w->stop))
                break;
            continue;
        }
        struct timeval tv = { .tv_sec = 5 };
        tv.tv_usec = 0;
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        serve_client(w, c);
        close(c);
    }
    return NULL;
}

/* ---- lifecycle ---- */

int tb_webui_start(tb_webui_t **out, uint16_t want_port,
                   const char *ledger_path, tb_error_t *err)
{
    if (!out || !ledger_path)
        TB_ERROR_FAIL(err, TB_DOMAIN_CORE, TB_CORE_INVALID_ARG,
                      "webui: аргументы");

    tb_webui_t *w = calloc(1, sizeof(*w));
    if (!w)
        TB_ERROR_FAIL(err, TB_DOMAIN_CORE, TB_CORE_NO_MEMORY, "webui");
    snprintf(w->ledger_path, sizeof(w->ledger_path), "%s", ledger_path);
    pthread_mutex_init(&w->lock, NULL);

    int bound = -1;
    for (int try = 0; try <= 20; try++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            break;
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        struct sockaddr_in a = {0};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons((uint16_t)(want_port + try));
        if (bind(fd, (struct sockaddr *)&a, sizeof(a)) == 0
            && listen(fd, 8) == 0) {
            bound = fd;
            w->port = (uint16_t)(want_port + try);
            break;
        }
        close(fd);
    }
    if (bound < 0) {
        pthread_mutex_destroy(&w->lock);
        free(w);
        TB_ERROR_FAIL(err, TB_DOMAIN_CORE, TB_CORE_IO,
                      "webui: нет свободного порта с %u", want_port);
    }
    w->fd = bound;
    tb_atomic_store_bool(&w->stop, false);
    if (pthread_create(&w->th, NULL, web_thread, w) != 0) {
        close(w->fd);
        pthread_mutex_destroy(&w->lock);
        free(w);
        TB_ERROR_FAIL(err, TB_DOMAIN_CORE, TB_CORE_IO, "webui: поток");
    }
    *out = w;
    return 0;
}

void tb_webui_set_json(tb_webui_t *w, const char *json)
{
    if (!w || !json)
        return;
    pthread_mutex_lock(&w->lock);
    snprintf(w->snapshot, sizeof(w->snapshot), "%s", json);
    pthread_mutex_unlock(&w->lock);
}

uint16_t tb_webui_port(const tb_webui_t *w)
{
    return w ? w->port : 0;
}

void tb_webui_stop(tb_webui_t *w)
{
    if (!w)
        return;
    tb_atomic_store_bool(&w->stop, true);
    shutdown(w->fd, SHUT_RDWR); /* будит accept() в потоке морды */
    close(w->fd);
    pthread_join(w->th, NULL);
    pthread_mutex_destroy(&w->lock);
    free(w);
}
