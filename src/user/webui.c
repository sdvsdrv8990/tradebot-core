#define TB_CURRENT_GROUP TB_G_ARB /* зона ответственности файла */

/* ==========================================================================
 *  tb/user/webui.c — веб-морда: страницы-микромодули + JSON API (v0.5).
 *
 *  Микромодульность: страница = {путь, заголовок, JS}, регистрируется
 *  приложением (какие — решает конфиг). Оболочка (тема, навигация,
 *  автообновление) одна; JS страницы сам фетчит свой /api/<имя> и
 *  рисует таблицу в #page. Данные в морде не живут: именованные
 *  снапшоты кладёт приложение из потока цикла (мьютекс), журнал читаем
 *  собственным read-only fd — гонок с ботом нет по построению.
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

#define WEBUI_SNAP_MAX   (8 * 1024)
#define WEBUI_PAGE_MAX   (32 * 1024)
#define WEBUI_LEDGER_TAIL (64 * 1024)

typedef struct {
    char name[16];
    char json[WEBUI_SNAP_MAX];
} snap_ent_t;

typedef struct tb_webui {
    int          fd;
    uint16_t     port;
    pthread_t    th;
    volatile bool stop;
    pthread_mutex_t lock;      /* snaps + pages */
    snap_ent_t   snaps[TB_WEBUI_SNAPS_MAX];
    uint32_t     snaps_n;
    tb_web_page_t pages[TB_WEBUI_PAGES_MAX];
    uint32_t     pages_n;
    char         ledger_path[512];
    char         logos_dir[380]; /* ./logos — локальные лого /logo/... */
} tb_webui_t;

/* ---- HTML-оболочка (тема + навигация + автообновление) ---- */

static const char SHELL_HEAD[] =
    "<!doctype html><html lang=\"ru\"><head><meta charset=\"utf-8\">"
    "<title>tradebot-core</title><style>"
    ":root{color-scheme:dark}body{font:14px/1.45 system-ui,sans-serif;"
    "background:#111318;color:#d7dce3;margin:0 0 40px}"
    ".wrap{max-width:1100px;margin:0 auto;padding:0 24px}"
    "h1{font-size:18px;margin:18px 0 2px}.sub{color:#7b8494;font-size:12px}"
    "nav{display:flex;gap:4px;margin:14px 0 4px;border-bottom:1px solid "
    "#262c37;padding-bottom:8px;flex-wrap:wrap}"
    "nav a{color:#9aa4b2;text-decoration:none;font-size:13px;padding:4px 12px;"
    "border-radius:6px}nav a.on{background:#1a1e26;color:#e8ecf1}"
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
    ".logo{width:18px;height:18px;border-radius:50%;vertical-align:-4px;"
    "margin-right:6px;object-fit:cover}"
    ".badge{display:inline-flex;width:18px;height:18px;border-radius:50%;"
    "background:#262c37;color:#9aa4b2;font-size:10px;align-items:center;"
    "justify-content:center;margin-right:6px;vertical-align:-4px}"
    "a.dex{color:inherit;text-decoration:none}a.dex:hover{text-decoration:"
    "underline}"
    ".tag{display:inline-block;padding:1px 7px;border-radius:10px;"
    "font-size:11px}.t-ok{background:#1d3a2f;color:#5dd39e}"
    ".t-err{background:#3a241f;color:#e07a5f}.t-pend{background:#33301d;"
    "color:#d9b45a}.mut{color:#7b8494}"
    "</style></head><body><div class=\"wrap\">"
    "<h1>tradebot-core</h1><div class=\"sub\" id=\"mode\">…</div>"
    "<nav id=\"nav\"></nav><div id=\"page\"></div></div><script>";

/* Общие JS-хелперы для всех страниц. */
static const char SHELL_JS[] =
    "const esc=s=>String(s==null?'':s).replace(/[&<>]/g,c=>({'&':'&amp;',"
    "'<':'&lt;','>':'&gt;'}[c]));"
    "const fmt=t=>new Date(t).toLocaleTimeString('ru-RU');"
    "const short=s=>{s=String(s||'');return s.length>12?s.slice(0,8)+'…'"
    "+s.slice(-4):s};"
    "const bp=n=>((n>0?'+':'')+ (Number(n)/100).toFixed(2)+'%');"
    "const big=s=>{try{return BigInt(s||0)}catch(e){return 0n}};"
    "const eth=w=>{const v=big(w),s=v<0n?-v:v,f=s/10n**10n;"
    "return (v<0n?'-':'')+(f/10n**8n).toString()+'.'+"
    "(f%10n**8n).toString().padStart(8,'0')};"
    "const DEXU={uniswap:'https://app.uniswap.org',"
    "pancakeswap:'https://pancakeswap.finance',"
    "sushiswap:'https://www.sushi.com/swap'};"
    "const slug=n=>String(n||'').toLowerCase().replace(/-v[23]$/,'');"
    "const logoErr=i=>{i.outerHTML='<span class=\"badge\">'+"
    "esc(i.dataset.f||'?').slice(0,1).toUpperCase()+'</span>'};"
    "const dexLogo=n=>'<img class=\"logo\" loading=\"lazy\" data-f=\"'+"
    "esc(n)+'\" onerror=\"logoErr(this)\" src=\"/logo/dex/'+slug(n)+"
    "'.webp\" alt=\"\">';"
    "const tokLogo=a=>'<img class=\"logo\" loading=\"lazy\" data-f=\"'+"
    "esc(a)+'\" onerror=\"logoErr(this)\" src=\"/logo/token/'+"
    "String(a||'').toLowerCase()+'.png\" alt=\"\">';"
    "const dexCell=x=>{const v=String(x||'').split(' ')[0];"
    "const fee=String(x||'').split(' ').slice(1).join(' ');"
    "const u=DEXU[slug(v)];"
    "const inner=dexLogo(v)+'<span title=\"'+esc(v)+'\">'+esc(fee||v)"
    "+'</span>';"
    "return u?('<a class=\"dex\" href=\"'+u+'\" target=\"_blank\" "
    "rel=\"noreferrer\">'+inner+'</a>'):inner};";

/* Собрать страницу: оболочка + навигация (активная подсвечена) + JS. */
static size_t build_page(tb_webui_t *w, char *out, size_t cap, uint32_t pi)
{
    size_t o = (size_t)snprintf(out, cap, "%s", SHELL_HEAD);
    o += (size_t)snprintf(out + o, cap - o, "%s", SHELL_JS);

    pthread_mutex_lock(&w->lock);
    o += (size_t)snprintf(out + o, cap - o,
                          "const NAV=[");
    for (uint32_t i = 0; i < w->pages_n; i++)
        o += (size_t)snprintf(out + o, cap - o,
                              "%s{p:\"%s\",t:\"%s\"}",
                              i ? "," : "", w->pages[i].path,
                              w->pages[i].title);
    o += (size_t)snprintf(out + o, cap - o, "];const ACT=%u;", pi);
    const char *js = w->pages[pi].js;
    pthread_mutex_unlock(&w->lock);

    o += (size_t)snprintf(out + o, cap - o,
        "const __last={};/* анти-мерцание: те же данные — не перерисовываем */"
    "const changed=(k,s)=>{if(__last[k]===s)return false;__last[k]=s;"
    "return true};"
    "document.getElementById('nav').innerHTML=NAV.map((n,i)=>"
        "'<a href=\"'+n.p+'\" class=\"'+(i===ACT?'on':'')+'\">'+n.t+'</a>')"
        ".join('');");
    o += (size_t)snprintf(out + o, cap - o, "%s", js);
    o += (size_t)snprintf(out + o, cap - o,
        "tick();setInterval(tick,2000);</script></body></html>");
    return o;
}

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

static void respond_404(int c)
{
    static const char nf[] = "HTTP/1.1 404 Not Found\r\n"
                             "Content-Length: 0\r\nConnection: close"
                             "\r\n\r\n";
    send_all(c, nf, sizeof(nf) - 1);
}

static void respond_named(tb_webui_t *w, int c, const char *name)
{
    char snap[WEBUI_SNAP_MAX];
    snap[0] = '\0';
    pthread_mutex_lock(&w->lock);
    for (uint32_t i = 0; i < w->snaps_n; i++)
        if (strcmp(w->snaps[i].name, name) == 0) {
            memcpy(snap, w->snaps[i].json, sizeof(snap));
            break;
        }
    pthread_mutex_unlock(&w->lock);
    if (!snap[0])
        snprintf(snap, sizeof(snap), "{\"_\":\"нет данных: %s\"}", name);
    respond(c, "application/json", snap, strlen(snap));
}

/* Хвост журнала: последние строки JSONL как массив объектов.
 * prefix != NULL — только записи с "type":"<prefix>…". */
static void respond_ledger(tb_webui_t *w, int c, const char *prefix)
{
    char pat[40] = "";
    if (prefix)
        snprintf(pat, sizeof(pat), "\"type\":\"%s", prefix);

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
        if (pat[0] && !strstr(line, pat))
            continue; /* не нужный тип записи */
        if (o + ll + 4 >= sizeof(out))
            break;
        o += (size_t)snprintf(out + o, sizeof(out) - o, "%s%s",
                              first ? "" : ",", line);
        first = 0;
    }
    snprintf(out + o, sizeof(out) - o, "]}");
    respond(c, "application/json", out, strlen(out));
}

/* Локальные логотипы: /logo/<kind>/<name>, kind = dex | token, файл из
 * каталога logos_dir (по умолчанию ./logos, задаётся перед start).
 * Имя строго [a-z0-9.-]: обход каталогов исключён. Нет файла — 404,
 * клиент рисует буквенный бейдж. */
static void respond_logo(tb_webui_t *w, int c, const char *kind,
                         const char *name)
{
    if (!kind || !name)
        return (void)respond_404(c);
    if (strcmp(kind, "dex") != 0 && strcmp(kind, "token") != 0)
        return (void)respond_404(c);
    for (const char *p = name; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9')
              || *p == '.' || *p == '-'))
            return (void)respond_404(c);
    if (strstr(name, ".."))
        return (void)respond_404(c);

    char file[600];
    snprintf(file, sizeof(file), "%s/%s/%s", w->logos_dir, kind, name);
    FILE *f = fopen(file, "rb");
    if (!f)
        return (void)respond_404(c);
    static char buf[64 * 1024];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    if (n == 0)
        return (void)respond_404(c);
    const char *ct = "image/png";
    const char *ext = strrchr(name, '.');
    if (ext) {
        if (strcmp(ext, ".webp") == 0)
            ct = "image/webp";
        else if (strcmp(ext, ".svg") == 0)
            ct = "image/svg+xml";
        else if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0)
            ct = "image/jpeg";
    }
    /* кэш браузера: логотипы не меняются — не мигают при перерисовках */
    static const char head[] =
        "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Cache-Control: max-age=86400\r\nConnection: close\r\n\r\n";
    char h[192];
    int hn = snprintf(h, sizeof(h), head, ct, n);
    if (hn > 0)
        send_all(c, h, (size_t)hn);
    send_all(c, buf, n);
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
    if (strncmp(req, "GET ", 4) != 0) {
        respond_404(c);
        return;
    }
    char path[64] = {0};
    for (size_t i = 0; i < sizeof(path) - 1 && req[4 + i] != ' '; i++)
        path[i] = req[4 + i];

    if (strncmp(path, "/logo/", 6) == 0) {
        char kind[16] = {0}, name[64] = {0};
        const char *rest = path + 6;
        const char *slash = strchr(rest, '/');
        if (slash && slash - rest < (ptrdiff_t)sizeof(kind)
            && strlen(slash + 1) < sizeof(name)) {
            memcpy(kind, rest, (size_t)(slash - rest));
            snprintf(name, sizeof(name), "%s", slash + 1);
            respond_logo(w, c, kind, name);
            return;
        }
        respond_404(c);
        return;
    }

    if (strncmp(path, "/api/", 5) == 0) {
        const char *name = path + 5;
        if (strcmp(name, "status") == 0)
            respond_named(w, c, "status");
        else if (strcmp(name, "ledger") == 0)
            respond_ledger(w, c, NULL);
        else if (strcmp(name, "trades") == 0)
            respond_ledger(w, c, "trade.");
        else
            respond_named(w, c, name);
        return;
    }

    /* HTML-страницы */
    uint32_t pi = UINT32_MAX;
    pthread_mutex_lock(&w->lock);
    for (uint32_t i = 0; i < w->pages_n; i++) {
        const char *p = w->pages[i].path;
        if (strcmp(path, p) == 0
            || (strcmp(p, "/") == 0 && strcmp(path, "/index.html") == 0)) {
            pi = i;
            break;
        }
    }
    pthread_mutex_unlock(&w->lock);
    if (pi == UINT32_MAX) {
        respond_404(c);
        return;
    }
    static char page[WEBUI_PAGE_MAX];
    size_t n = build_page(w, page, sizeof(page), pi);
    respond(c, "text/html; charset=utf-8", page, n);
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
    snprintf(w->logos_dir, sizeof(w->logos_dir), "%s", "./logos");
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

int tb_webui_page_add(tb_webui_t *w, const tb_web_page_t *page)
{
    if (!w || !page || !page->path || !page->js)
        return -1;
    int rc = -1;
    pthread_mutex_lock(&w->lock);
    if (w->pages_n < TB_WEBUI_PAGES_MAX) {
        w->pages[w->pages_n++] = *page; /* строки статические — shallow ok */
        rc = 0;
    }
    pthread_mutex_unlock(&w->lock);
    return rc;
}

int tb_webui_set_named(tb_webui_t *w, const char *name, const char *json)
{
    if (!w || !name || !json || strlen(name) >= sizeof(w->snaps[0].name))
        return -1;
    int rc = -1;
    pthread_mutex_lock(&w->lock);
    snap_ent_t *e = NULL;
    for (uint32_t i = 0; i < w->snaps_n; i++)
        if (strcmp(w->snaps[i].name, name) == 0)
            e = &w->snaps[i];
    if (!e && w->snaps_n < TB_WEBUI_SNAPS_MAX) {
        e = &w->snaps[w->snaps_n++];
        snprintf(e->name, sizeof(e->name), "%s", name);
    }
    if (e) {
        snprintf(e->json, sizeof(e->json), "%s", json);
        rc = 0;
    }
    pthread_mutex_unlock(&w->lock);
    return rc;
}

void tb_webui_set_json(tb_webui_t *w, const char *json)
{
    if (w && json)
        (void)tb_webui_set_named(w, "status", json);
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
