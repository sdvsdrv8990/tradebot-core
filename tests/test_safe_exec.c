/* ==========================================================================
 *  tests/test_safe_exec.c — безключевой propose-контур Safe на локальном
 *  mock (обычный HTTP, 127.0.0.1): calldata (exactInput/approve/
 *  execTransaction), предложение БЕЗ ключа и подписи, публикация в
 *  очередь (Transaction Service, POST /api/v1 -> 201), дисциплина
 *  nonce/allowance (гейт -> approve-предложение; одно живое предложение;
 *  замена того же nonce; рост nonce -> дельта баланса; истечение окна).
 *  eth_sendRawTransaction не вызывается ни разу; приватных ключей нет.
 * ========================================================================== */
#define TB_CURRENT_GROUP TB_G_DEMO

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "harness.h"
#include "tb/arch/arch.h"
#include "tb/core/error.h"
#include "tb/core/keccak.h"
#include "tb/core/time.h"
#include "tb/exec/backend_dex.h"
#include "tb/exec/safe_exec.h"
#include "tb/net/http.h"

static tb_addr_t A(const char *hex)
{
    tb_addr_t a;
    CHECK(tb_addr_from_hex(&a, hex, NULL) == 0);
    return a;
}

static tb_addr_t SAFE = { { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11,
                            0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11,
                            0x11, 0x11, 0x11, 0x11 } };
static tb_addr_t ROUTER = { { 0x68, 0xb3, 0x46, 0x58, 0x33, 0xfb, 0x72, 0xa7,
                              0x0e, 0xcd, 0xf4, 0x85, 0xe0, 0xe4, 0xc7, 0xbd,
                              0x86, 0x65, 0xfc, 0x45 } }; /* SwapRouter02 */
static tb_addr_t WETH = { { 0xc0, 0x2a, 0xaa, 0x39, 0xb2, 0x23, 0xfe, 0x8d,
                            0x0a, 0x0e, 0x5c, 0x4f, 0x27, 0xea, 0xd9, 0x08,
                            0x3c, 0x75, 0x6c, 0xc2 } };

/* ---- mock: JSON-RPC + REST очереди (POST /api/v1 -> 201) ---- */

typedef struct mock {
    int fd, port;
    pthread_t th;
    volatile bool stop;
    int sendraw_count;   /* попыток отправки tx: обязано быть НУЛЁМ */
    int service_posts;   /* публикаций в очередь Safe UI */
    int service_429;     /* N первых постов отвечать 429 (тариф) */
    uint64_t safe_nonce; /* nonce(); тест «исполняет» движением */
    uint64_t balance;    /* balanceOf */
    uint64_t allowance;  /* allowance(WETH, Safe -> router) */
    int logs_mode;       /* getLogs: 0 пусто, 1 Success, 2 Failure */
    char want_hash[67];  /* topic1 из запроса (эхо в ответе) */
    char want_topic0[67];/* topic0 исхода (тест считает keccak) */
    char ok_topic0[67], fail_topic0[67];
    uint64_t got_from;   /* fromBlock из запроса getLogs (борн-блок) */
} mock_t;

static void serve(mock_t *m, int c)
{
    char req[262144] = {0};
    size_t have = 0;
    while (have < sizeof(req) - 1) {
        ssize_t n = recv(c, req + have, sizeof(req) - 1 - have, 0);
        if (n <= 0)
            break;
        have += (size_t)n;
        char *h = strstr(req, "\r\n\r\n");
        if (h)
            break;
    }
    const char *body = strstr(req, "\r\n\r\n");
    if (!body) {
        close(c);
        return;
    }
    body += 4;

    char out[1024];
    int n = 0;
    const char *hdr_extra = "";

    if (strncmp(req, "POST /api/v1/", 13) == 0) {
        /* Safe Transaction Service: предложение в очередь */
        m->service_posts++;
        if (m->service_429 > 0) {
            m->service_429--;
            n = 0;
            char h4[192];
            int h4n = snprintf(h4, sizeof(h4),
                               "HTTP/1.1 429 Too Many Requests\r\n"
                               "Content-Type: application/json\r\n"
                               "Retry-After: 2\r\nContent-Length: 0\r\n"
                               "Connection: close\r\n\r\n");
            send(c, h4, (size_t)h4n, 0);
            close(c);
            return;
        }
        n = snprintf(out, sizeof(out), "{}");
        hdr_extra = "X-Queued: 1";
    } else if (strstr(body, "eth_getLogs")) {
        /* fromBlock запроса — проверка born_block (ревью L1) */
        {
            const char *f = strstr(body, "\"fromBlock\":\"0x");
            m->got_from = f ? strtoull(f + 15, NULL, 16) : 0;
        }
        /* topic1 (искомый хеш) — после "]," в topics */
        {
            const char *w = strstr(body, "],\"0x");
            snprintf(m->want_hash, sizeof(m->want_hash), "%s", "0x");
            if (w) {
                /* w+3='0', w+4='x' -> hex с w+5 */
                strncat(m->want_hash, w + 5, 64);
                m->want_hash[66] = '\0';
            }
        }
        if (m->logs_mode == 3) {
            n = snprintf(out, sizeof(out),
                         "{\"id\":1,\"error\":{\"code\":-32603,"
                         "\"message\":\"rpc: logs unavailable\"}}");
            goto serve_out;
        }
        snprintf(m->want_topic0, sizeof(m->want_topic0), "%s",
                 m->logs_mode == 2 ? m->fail_topic0 : m->ok_topic0);
        if (m->logs_mode == 0) {
            n = snprintf(out, sizeof(out), "{\"id\":1,\"result\":[]}");
        } else {
            /* topic0 по режиму; реальные топики посчитает тест через
             * tb_keccak256 и подсунет через want_topic0 */
            n = snprintf(out, sizeof(out),
                         "{\"id\":1,\"result\":[{\"address\":\"0x%.40s\","
                         "\"topics\":[\"%s\",\"%s\"],"
                         "\"blockNumber\":\"0x9\",\"data\":\"0x\"}]}",
                         "1111111111111111111111111111111111111111",
                         m->want_topic0, m->want_hash);
        }
    } else if (strstr(body, "eth_blockNumber")) {
        n = snprintf(out, sizeof(out), "{\"id\":1,\"result\":\"0x10\"}");
    } else if (strstr(body, "eth_sendRawTransaction")) {
        m->sendraw_count++;
        n = snprintf(out, sizeof(out),
                     "{\"id\":1,\"error\":{\"code\":-1,"
                     "\"message\":\"propose-only: отправка запрещена\"}}");
    } else if (strstr(body, "eth_chainId")) {
        n = snprintf(out, sizeof(out), "{\"id\":1,\"result\":\"0x1\"}");
    } else if (strstr(body, "eth_call")) {
        /* селекторы: nonce() 1d6da664 / balanceOf 70a08231 /
         * allowance dd62ed3e */
        if (body && strstr(body, "1d6da664"))
            n = snprintf(out, sizeof(out),
                         "{\"id\":1,\"result\":\"0x%064llx\"}",
                         (unsigned long long)m->safe_nonce);
        else if (body && strstr(body, "dd62ed3e"))
            n = snprintf(out, sizeof(out),
                         "{\"id\":1,\"result\":\"0x%064llx\"}",
                         (unsigned long long)m->allowance);
        else
            n = snprintf(out, sizeof(out),
                         "{\"id\":1,\"result\":\"0x%064llx\"}",
                         (unsigned long long)m->balance);
    } else {
        n = snprintf(out, sizeof(out), "{\"id\":1,\"error\":{\"code\":-1}}");
    }

    char hdr[192];
serve_out:
    {
        int hn = snprintf(hdr, sizeof(hdr),
                          "HTTP/1.1 200 OK\r\nContent-Type: "
                          "application/json\r\n%s%sContent-Length: %zu"
                          "\r\nConnection: close\r\n\r\n",
                          *hdr_extra ? hdr_extra : "",
                          *hdr_extra ? "\r\n" : "", strlen(out));
        send(c, hdr, (size_t)hn, 0);
        send(c, out, (size_t)n, 0);
    }
    close(c);
}

static void *mock_thread(void *arg)
{
    mock_t *m = arg;
    while (!tb_atomic_load_bool(&m->stop)) {
        int c = accept(m->fd, NULL, NULL);
        if (c < 0)
            break;
        serve(m, c);
    }
    return NULL;
}

static void mock_start(mock_t *m)
{
    m->stop = false;
    m->sendraw_count = 0;
    m->service_posts = 0;
    m->safe_nonce = 5;
    m->balance = 1000;
    m->allowance = 0;   /* старт без approve: гейт должен сработать */
    m->logs_mode = 0;
    m->service_429 = 0;
    {
        /* topic0 событий Safe — по-настоящему, keccak подписей */
        uint8_t t[32];
        tb_keccak256("ExecutionSuccess(bytes32,uint256)",
                     sizeof("ExecutionSuccess(bytes32,uint256)") - 1, t);
        for (int i = 0; i < 32; i++)
            sprintf(m->ok_topic0 + 2 + i * 2, "%02x", t[i]);
        tb_keccak256("ExecutionFailure(bytes32,uint256)",
                     sizeof("ExecutionFailure(bytes32,uint256)") - 1, t);
        for (int i = 0; i < 32; i++)
            sprintf(m->fail_topic0 + 2 + i * 2, "%02x", t[i]);
        m->ok_topic0[0] = m->fail_topic0[0] = '0';
        m->ok_topic0[1] = m->fail_topic0[1] = 'x';
    }
    m->fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    bind(m->fd, (struct sockaddr *)&a, sizeof(a));
    listen(m->fd, 8);
    struct sockaddr_in got = {0};
    socklen_t gl = sizeof(got);
    getsockname(m->fd, (struct sockaddr *)&got, &gl);
    m->port = ntohs(got.sin_port);
    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(m->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    pthread_create(&m->th, NULL, mock_thread, m);
}

static void mock_stop(mock_t *m)
{
    tb_atomic_store_bool(&m->stop, true);
    shutdown(m->fd, SHUT_RDWR);
    pthread_join(m->th, NULL);
    close(m->fd);
}


/* ---- декод MultiSend-пачки (тест R2) ---- */
typedef struct dec_call {
    tb_addr_t to;
    const uint8_t *data;
    size_t len;
} dec_call_t;

static size_t decode_multisend(const uint8_t *buf, size_t cap,
                               dec_call_t *calls, size_t max)
{
    uint8_t sel[4];
    tb_keccak_selector("multiSend(bytes)", sel);
    if (buf[0] != sel[0] || buf[1] != sel[1] || buf[2] != sel[2]
        || buf[3] != sel[3])
        return 0;
    /* offset(32) = 0x20, len(32), payload */
    size_t o = 4;
    uint64_t off = 0, plen = 0;
    for (int i = 0; i < 8; i++)
        off = (off << 8) | buf[o + 24 + i];
    o += 32;
    for (int i = 0; i < 8; i++)
        plen = (plen << 8) | buf[o + 24 + i];
    o += 32;
    (void)off;
    size_t end = o + plen;
    size_t n = 0;
    while (o < end && n < max) {
        uint8_t op = buf[o++];
        (void)op;
        memcpy(calls[n].to.b, buf + o, 20);
        o += 20;
        uint64_t val = 0, dlen = 0;
        for (int i = 0; i < 8; i++)
            val = (val << 8) | buf[o + 24 + i];
        o += 32;
        for (int i = 0; i < 8; i++)
            dlen = (dlen << 8) | buf[o + 24 + i];
        o += 32;
        (void)val;
        calls[n].data = buf + o;
        calls[n].len = dlen;
        o += dlen;
        n++;
    }
    return n;
}

/* exactInput слово количества по индексу головы (0=amountIn,1=minOut) */
static uint64_t dec_exact_input_word(const dec_call_t *c, int which)
{
    /* голова: offset(0)=4..36, recipient=4+32, amountIn=4+64,
     * minOut=4+96 (по 32 байта BE) */
    size_t off = 4 + 32 + 32 * (1 + which);
    uint64_t v = 0;
    for (int i = 24; i < 32; i++)
        v = (v << 8) | c->data[off + i];
    return v;
}

/* path-хоп exactInput: fee-слово хопа k (после хвоста len+path) */
static uint32_t dec_path_fee(const dec_call_t *c, int hop)
{
    /* path: token(20) fee(3) token(20) fee(3)... — fee хопа k после
     * 20 байт его токена: tail + 20 + hop*23 */
    size_t tail = 4 + 32 * 4 + 32;
    size_t p = tail + 20 + hop * 23;
    return (uint32_t)c->data[p] << 16 | (uint32_t)c->data[p + 1] << 8
           | c->data[p + 2];
}

static int dec_path_token(const dec_call_t *c, int idx, tb_addr_t *out)
{
    size_t tail = 4 + 32 * 4 + 32;
    size_t p = tail + idx * 23;
    if (p + 20 > c->len)
        return -1;
    memcpy(out->b, c->data + p, 20);
    return 0;
}

/* улов предложений */
static tb_safe_proposal_t g_props[8];
static int g_prop_n;
static void catch_proposal(const tb_safe_proposal_t *p, void *user)
{
    (void)user;
    if (g_prop_n < 8)
        g_props[g_prop_n++] = *p;
}

/* улов атрибуций исполнения (H1.5) */
static tb_exec_dex_execution_t g_execs[4];
static int g_exec_n;
static void catch_execution(const tb_exec_dex_execution_t *e, void *user)
{
    (void)user;
    if (g_exec_n < 4)
        g_execs[g_exec_n++] = *e;
}

int main(void)
{
    tb_error_t err = TB_ERROR_INIT;
    mock_t mock;
    mock_start(&mock);

    tb_http_t *http = tb_http_net_create("test", NULL, &err);
    CHECK(http != NULL);

    /* ---- 1. Calldata: exactInput ---- */
    {
        tb_addr_t usdc = A("0xA0b86991c6218b36c1d19D4a2e9Eb0cE3606eB48");
        tb_route_t r = tb_route_single(&WETH, &usdc, 500, &err);
        tb_addr_t pool = A("0x88e6A0c2dDD26FEEb64F039a2c41296FcB3f5640");
        tb_route_set_pool(&r, 0, &pool);
        uint8_t buf[512];
        tb_u256_t in, mn;
        tb_u256_from_dec(&in, "1000000000000000", 16, NULL);
        tb_u256_from_dec(&mn, "2250000", 7, NULL);
        size_t n = tb_safe_build_exact_input(buf, sizeof(buf), &r, &SAFE,
                                             &in, &mn);
        CHECK(n > 4 + 32 * 4 + 32 + 43);
        CHECK(buf[0] == 0xb8 && buf[1] == 0x58 && buf[2] == 0x18
              && buf[3] == 0x3f);
        CHECK(memcmp(buf + 4 + 32 * 1 + 12, SAFE.b, 20) == 0);
    }

    /* ---- 2. Calldata: approve ---- */
    {
        uint8_t buf[4 + 64];
        tb_u256_t amt;
        tb_u256_from_dec(&amt, "1000000000000000000", 19, NULL);
        size_t n = tb_safe_build_approve(buf, sizeof(buf), &ROUTER, &amt);
        CHECK(n == 4 + 64);
        CHECK(buf[0] == 0x09 && buf[1] == 0x5e && buf[2] == 0xa7
              && buf[3] == 0xb3);
        CHECK(memcmp(buf + 4 + 12, ROUTER.b, 20) == 0);
    }

    /* ---- 3. Calldata: execTransaction (универсальная сборка) ---- */
    {
        uint8_t data[64] = { 0xde, 0xad, 0xbe, 0xef };
        uint8_t sig[65];
        memset(sig, 0xAA, 65);
        uint8_t buf[640];
        size_t n = tb_safe_build_exec_tx(buf, sizeof(buf), &ROUTER, 0, data,
                                         4, sig);
        CHECK(buf[0] == 0x6a && buf[1] == 0x76 && buf[2] == 0x12
              && buf[3] == 0x02);
        CHECK(memcmp(buf + 4 + 12, ROUTER.b, 20) == 0);
        CHECK(n > 4 + 32 * 10);
    }

    /* ---- 4. Предложение БЕЗ ключа: подписи нет, verify есть ---- */
    {
        tb_safe_cfg_t cfg = {0};
        snprintf(cfg.rpc_url, sizeof(cfg.rpc_url), "http://127.0.0.1:%u",
                 (unsigned)mock.port);
        cfg.safe = SAFE;

        tb_safe_proposal_t p;
        uint8_t cd[4 + 64];
        tb_u256_t amt;
        tb_u256_from_dec(&amt, "1000000000000000", 16, NULL);
        size_t cd_len = tb_safe_build_approve(cd, sizeof(cd), &ROUTER,
                                              &amt);
        CHECK(tb_safe_propose_exec(http, &cfg, &WETH, 0, cd, cd_len,
                                   tb_now_ms(), &p, &err)
              == 0);
        CHECK(p.chain_id == 1);
        CHECK(p.safe_nonce == 5);
        CHECK(p.expires_at_ms > p.created_ms);
        CHECK(strcmp(p.kind, "trade") == 0);
        CHECK(p.data_len == cd_len);

        /* JSON: без bot_signature; kind и verify присутствуют */
        char js[1600];
        size_t jn = tb_safe_proposal_json(&p, js, sizeof(js));
        CHECK(jn > 100);
        CHECK(strstr(js, "\"bot_signature\"") == NULL);
        CHECK(strstr(js, "\"kind\":\"trade\"") != NULL);
        CHECK(strstr(js, "\"nonce\":5") != NULL);
    }

    /* ---- 5. Публикация в очередь (service, мок на том же порту) ---- */
    {
        tb_safe_cfg_t cfg = {0};
        snprintf(cfg.rpc_url, sizeof(cfg.rpc_url), "http://127.0.0.1:%u",
                 (unsigned)mock.port);
        snprintf(cfg.tx_service_url, sizeof(cfg.tx_service_url),
                 "http://127.0.0.1:%u", (unsigned)mock.port);
        cfg.safe = SAFE;

        tb_safe_proposal_t p;
        uint8_t cd[4] = { 0x09, 0x5e, 0xa7, 0xb3 };
        CHECK(tb_safe_propose_exec(http, &cfg, &WETH, 0, cd, sizeof(cd),
                                   tb_now_ms(), &p, &err)
              == 0);
        int posts = mock.service_posts;
        CHECK(tb_safe_service_propose(http, &cfg, &p, &err) == 0);
        CHECK(mock.service_posts == posts + 1); /* POST /api/v1 был */

        /* H1.6: тарифный лимит — 429 читаем однозначно, Retry-After
         * доходит до ошибки; клиент НЕ ограничивает сам */
        mock.service_429 = 1;
        tb_error_t r4 = TB_ERROR_INIT;
        CHECK(tb_safe_service_propose(http, &cfg, &p, &r4) == -1);
        CHECK(strstr(r4.message, "лимит запросов") != NULL);
        CHECK(r4.retryable == true);
        CHECK(r4.retry_after_ms == 2000); /* Retry-After: 2 сек */
        mock.service_429 = 0;
    }

    /* ---- 6. Полный live-цикл: гейт -> approve -> подпись -> trade ---
     * ---- замена -> исполнение -> дельта; истечение ---- */
    {
        tb_exec_dex_cfg_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        strcpy(cfg.venues[0].name, "uni-test");
        snprintf(cfg.venues[0].rpc_url, sizeof(cfg.venues[0].rpc_url),
                 "http://127.0.0.1:%u", (unsigned)mock.port);
        cfg.venues[0].quoter = A("0x61fFE014bA17989E743c5F6cB21bF9697530B21e");
        cfg.venues[0].factory =
            A("0x1F98431c8aD98523631AE4a59f267346ea31F984"); /* Uni V3 —
            та же фабрика, что резолвит роутер исполнения */
        cfg.venues[0].pools[0] = A("0x88e6A0c2dDD26FEEb64F039a2c41296FcB3f5640");
        cfg.venues[0].pools_n = 1;
        cfg.venues_n = 1;
        cfg.seed = 42;
        cfg.live = true;
        snprintf(cfg.safe.rpc_url, sizeof(cfg.safe.rpc_url),
                 "http://127.0.0.1:%u", (unsigned)mock.port);
        cfg.safe.safe = SAFE;
        cfg.safe.router = ROUTER;
        cfg.safe.chain_id = 1;
        cfg.approve_cap_wei = 10000000000000000ull;
        cfg.on_propose = catch_proposal;
        cfg.on_execution = catch_execution;

        tb_exec_backend_t b;
        CHECK(tb_exec_dex_init(&b, http, &cfg, &err) == 0);

        tb_addr_t usdc = A("0xA0b86991c6218b36c1d19D4a2e9Eb0cE3606eB48");
        tb_route_t r = tb_route_single(&WETH, &usdc, 500, &err);
        tb_route_set_pool(&r, 0, &cfg.venues[0].pools[0]);
        tb_u256_t in, mn, got;
        tb_u256_from_dec(&in, "1000000000000000", 16, NULL);
        tb_u256_from_dec(&mn, "900", 3, NULL);
        uint64_t tx = 0;

        /* 6a. allowance = 0: trade отказан, предложен approve */
        g_prop_n = 0;
        tb_error_t serr = TB_ERROR_INIT;
        int rc = b.submit(&b, &r, &in, &mn, 0, tb_now_ms(), &tx, &got,
                          &serr);
        CHECK(rc == -1);
        CHECK(strstr(serr.message, "allowance") != NULL);
        CHECK(g_prop_n == 1);
        CHECK(strcmp(g_props[0].kind, "approve") == 0);
        CHECK(mock.sendraw_count == 0);

        /* 6b. allowance всё ещё 0: approve уже жив — trade отказан
         *     без повторного approve-предложения */
        g_prop_n = 0;
        rc = b.submit(&b, &r, &in, &mn, 0, tb_now_ms(), &tx, &got, &serr);
        CHECK(rc == -1);
        CHECK(g_prop_n == 0); /* повтор не нужен — тот же approve жив */

        /* 6c. «человек подписал approve»: allowance вырос, nonce тоже */
        mock.allowance = 2000000000000000000ull;
        mock.safe_nonce = 6;
        g_prop_n = 0;
        rc = b.submit(&b, &r, &in, &mn, 0, tb_now_ms(), &tx, &got, &serr);
        CHECK(rc == 1);
        CHECK(tx != 0);
        CHECK(g_prop_n == 1);
        CHECK(strcmp(g_props[0].kind, "trade") == 0);
        CHECK(g_props[0].safe_nonce == 6);
        uint64_t tx1 = tx;

        /* 6d. poll: nonce не двигался — ждём */
        tb_error_t perr = TB_ERROR_INIT;
        CHECK(b.poll(&b, tx1, tb_now_ms(), &got, &perr) == 1);

        /* 6e. второе предложение до исполнения заменяет первое */
        g_prop_n = 0;
        rc = b.submit(&b, &r, &in, &mn, 0, tb_now_ms(), &tx, &got, &serr);
        CHECK(rc == 1);
        CHECK(g_prop_n == 1);
        CHECK(g_props[0].safe_nonce == 6); /* тот же nonce */
        uint64_t tx2 = tx;
        tb_error_t oerr = TB_ERROR_INIT;
        CHECK(b.poll(&b, tx1, tb_now_ms(), &got, &oerr) == -1);
        CHECK(strstr(oerr.message, "заменено") != NULL);

        /* 6f. «исполнение»: nonce вырос, баланс сместился; атрибуция
         *     H1.5 срабатывает ВНУТРИ poll — мок уже должен отвечать
         *     событием Success нашего хеша (эхо topic1) */
        mock.safe_nonce = 7;
        mock.balance = 1500;
        mock.logs_mode = 1;
        CHECK(b.poll(&b, tx2, tb_now_ms(), &got, &perr) == 0);
        CHECK(got.w[0] == 500); /* дельта 1500-1000 */
        CHECK(g_exec_n == 1);
        CHECK(g_execs[0].status == TB_ATTRIB_FOUND);
        CHECK(g_execs[0].success == true);
        CHECK(g_execs[0].block == 9);
        CHECK(g_execs[0].balance_delta.w[0] == 500);
        /* getLogs шёл от блока рождения (born_block=16: мок отвечает
         * eth_blockNumber=0x10 при submit), не от «текущий-4096» */
        CHECK(mock.got_from == 16);

        /* 6h. отказ исполнения: событие Failure нашего хеша */
        g_exec_n = 0;
        rc = b.submit(&b, &r, &in, &mn, 0, tb_now_ms(), &tx, &got, &serr);
        CHECK(rc == 1);
        mock.safe_nonce = 9;
        mock.balance = 1400; /* дельта 1400-1500 < 0 -> 0 */
        mock.logs_mode = 2;
        tb_error_t ferr2 = TB_ERROR_INIT;
        CHECK(b.poll(&b, tx, tb_now_ms(), &got, &ferr2) == 0);
        CHECK(g_exec_n == 1);
        CHECK(g_execs[0].status == TB_ATTRIB_FOUND);
        CHECK(g_execs[0].success == false); /* safe.rejected путь */

        /* 6i. nonce вырос, но события НАШЕГО хеша нет: исполнилось
         *     другое предложение Safe — атрибуция не доказана */
        g_exec_n = 0;
        rc = b.submit(&b, &r, &in, &mn, 0, tb_now_ms(), &tx, &got, &serr);
        CHECK(rc == 1);
        mock.safe_nonce = 11;
        mock.logs_mode = 0; /* пустой ответ логов */
        tb_error_t ferr3 = TB_ERROR_INIT;
        CHECK(b.poll(&b, tx, tb_now_ms(), &got, &ferr3) == 0);
        CHECK(g_exec_n == 1);
        CHECK(g_execs[0].status == TB_ATTRIB_ABSENT); /* unattributed:
                                                        * исполнилось
                                                        * другое, НЕ отказ */

        /* 6g. истечение окна: отказ без цепи */
        g_prop_n = 0;
        rc = b.submit(&b, &r, &in, &mn, 0, tb_now_ms(), &tx, &got, &serr);
        CHECK(rc == 1);
        tb_error_t eerr = TB_ERROR_INIT;
        CHECK(b.poll(&b, tx, tb_now_ms() + TB_SAFE_PROPOSAL_WATCH_MS + 1000,
                     &got, &eerr)
              == -1);
        CHECK(strstr(eerr.message, "истекло") != NULL);
        CHECK(mock.sendraw_count == 0); /* ни одной отправки за всё */

        /* 6j. RPC не ответил на getLogs: проверить не удалось — это
         * ERROR, не ABSENT; колбэк вызывается всегда (ревью L1:
         * иначе журнал терял бы даже факт неудавшейся проверки) */
        g_exec_n = 0;
        rc = b.submit(&b, &r, &in, &mn, 0, tb_now_ms(), &tx, &got, &serr);
        CHECK(rc == 1);
        mock.safe_nonce = 13;
        mock.logs_mode = 3;
        tb_error_t ferr4 = TB_ERROR_INIT;
        CHECK(b.poll(&b, tx, tb_now_ms(), &got, &ferr4) == 0);
        CHECK(g_exec_n == 1);
        CHECK(g_execs[0].status == TB_ATTRIB_ERROR);

        /* 6m. R2: кросс-DEX через MultiSendCallOnly — обе биржи в
         *     одной транзакции, каждая нога своим роутером; пачка
         *     декодируется, селектор сверен keccak, минимумы цепочкой */
        {
            tb_exec_dex_cfg_t cfg4;
            memset(&cfg4, 0, sizeof(cfg4));
            strcpy(cfg4.venues[0].name, "uni-test");
            snprintf(cfg4.venues[0].rpc_url, sizeof(cfg4.venues[0].rpc_url),
                     "http://127.0.0.1:%u", (unsigned)mock.port);
            cfg4.venues[0].quoter =
                A("0x61fFE014bA17989E743c5F6cB21bF9697530B21e");
            cfg4.venues[0].factory =
                A("0x1F98431c8aD98523631AE4a59f267346ea31F984");
            cfg4.venues[0].pools[0] =
                A("0x88e6A0c2dDD26FEEb64F039a2c41296FcB3f5640"); /* WETH/USDC uni */
            cfg4.venues[0].pools_n = 1;
            strcpy(cfg4.venues[1].name, "pancake-test");
            snprintf(cfg4.venues[1].rpc_url, sizeof(cfg4.venues[1].rpc_url),
                     "http://127.0.0.1:%u", (unsigned)mock.port);
            cfg4.venues[1].quoter =
                A("0xB048Bbc1Ee6b733FFfCFb9e9CeF7375518e25997");
            cfg4.venues[1].factory =
                A("0x0BFbCF9fa4f9C56B0F40a671Ad40E0805A091865"); /* чужая */
            cfg4.venues[1].router =
                A("0x1b81D678ffb9C0263b24A97847620C99d213eB14"); /* свой */
            cfg4.venues[1].pools[0] =
                A("0x1ac1A8FEaAEa1900C4166dEeed0C11cC10669D36"); /* USDC/WETH pan */
            cfg4.venues[1].pools_n = 1;
            cfg4.venues_n = 2;
            cfg4.seed = 9;
            cfg4.live = true;
            snprintf(cfg4.safe.rpc_url, sizeof(cfg4.safe.rpc_url),
                     "http://127.0.0.1:%u", (unsigned)mock.port);
            cfg4.safe.safe = SAFE;
            cfg4.safe.router = ROUTER; /* роутер фабрики Uni */
            cfg4.safe.chain_id = 1;
            cfg4.approve_cap_wei = 10000000000000000ull;
            cfg4.on_propose = catch_proposal;

            tb_exec_backend_t b4;
            CHECK(tb_exec_dex_init(&b4, http, &cfg4, &err) == 0);

            tb_addr_t toks4[3] = { WETH, usdc, WETH };
            uint32_t fees4[2] = { 500, 500 };
            tb_route_t r4 = tb_route_encode(toks4, fees4, 3, &err);
            tb_route_set_pool(&r4, 0, &cfg4.venues[0].pools[0]);
            tb_route_set_pool(&r4, 1, &cfg4.venues[1].pools[0]);
            CHECK(r4.len > 0);

            mock.allowance = 2000000000000000000ull;
            g_prop_n = 0;
            tb_error_t xerr4 = TB_ERROR_INIT;
            int rc4 = b4.submit(&b4, &r4, &in, &mn, 0, tb_now_ms(), &tx,
                                &got, &xerr4);
            if (rc4 != 1)
                printf("6m rc=%d: %s\n", rc4, xerr4.message);
            CHECK(rc4 == 1);
            CHECK(g_prop_n == 1);
            CHECK(mock.sendraw_count == 0);

            /* цель execTransaction — MultiSendCallOnly, не роутер */
            char to_h[43];
            tb_addr_to_hex(&g_props[0].to, to_h, sizeof(to_h));
            CHECK(strcasecmp(to_h, "0x40A50Cf359f9E7d0A06deBa8f7c901E4dE094f98")
                  == 0);

            /* пачка: 2 вызова, каждый exactInput своим роутером */
            dec_call_t dc[4];
            size_t dn = decode_multisend(g_props[0].data,
                                         g_props[0].data_len, dc, 4);
            CHECK(dn == 2);
            CHECK(tb_addr_eq(&dc[0].to, &ROUTER)); /* фабрика роутера */
            CHECK(tb_addr_eq(&dc[1].to, &cfg4.venues[1].router));
            for (size_t q = 0; q < dn; q++) {
                CHECK(dc[q].data[0] == 0xb8 && dc[q].data[1] == 0x58
                      && dc[q].data[2] == 0x18 && dc[q].data[3] == 0x3f);
            }
            /* пути: WETH|500|USDC и USDC|500|WETH — fee-тиры на месте */
            CHECK(dec_path_fee(&dc[0], 0) == 500);
            CHECK(dec_path_fee(&dc[1], 0) == 500);
            tb_addr_t t0, t1, t2;
            CHECK(dec_path_token(&dc[0], 0, &t0) == 0);
            CHECK(dec_path_token(&dc[0], 1, &t1) == 0);
            CHECK(tb_addr_eq(&t0, &WETH));
            CHECK(tb_addr_eq(&t1, &usdc));
            CHECK(dec_path_token(&dc[1], 0, &t2) == 0);
            CHECK(tb_addr_eq(&t2, &usdc));
            /* минимумы: amountIn ноги 2 == ожидаемому выходу ноги 1;
             * финальный минимум == наш order min_out (900 в тесте) */
            uint64_t amt0 = dec_exact_input_word(&dc[0], 0);
            uint64_t amt1 = dec_exact_input_word(&dc[1], 0);
            CHECK(amt0 == 1000000000000000ull); /* ставка */
            CHECK(amt1 > 0 && amt1 < amt0 * 3000); /* USDC-масштаб */
            uint64_t mn1 = dec_exact_input_word(&dc[1], 1);
            CHECK(mn1 == 900); /* пол ордера — на последней ноге */
            uint64_t mn0 = dec_exact_input_word(&dc[0], 0);
            (void)mn0;
            /* промежуточный минимум > 0 и < ожидаемого выхода ноги 1 */
            uint64_t mn_mid = dec_exact_input_word(&dc[0], 1);
            CHECK(mn_mid > 0);
            CHECK(mn_mid < amt1);

            b4.destroy(&b4);
        }

        /* 6k. ЦЕЛОСТНОСТЬ МАРШРУТА: пул чужой фабрики (Pancake) —
         * роутер исполнения свернёт path в ДРУГОЙ пул; предложение
         * обязано не строиться (ревью L1, до Multicall3/R2) */
        {
            tb_exec_dex_cfg_t cfg2;
            memset(&cfg2, 0, sizeof(cfg2));
            cfg2 = cfg;
            cfg2.venues[0].factory =
                A("0x0BFbCF9fa4f9C56B0F40a671Ad40E0805A091865"); /* Pancake */
            cfg2.venues_n = 1;
            cfg2.on_propose = catch_proposal;
            cfg2.on_execution = catch_execution;
            tb_exec_backend_t b2;
            CHECK(tb_exec_dex_init(&b2, http, &cfg2, &err) == 0);
            g_prop_n = 0;
            tb_error_t xerr = TB_ERROR_INIT;
            int rc2 = b2.submit(&b2, &r, &in, &mn, 0, tb_now_ms(), &tx,
                                &got, &xerr);
            CHECK(rc2 == -1);
            CHECK(strstr(xerr.message, "чужой фабрике") != NULL);
            CHECK(g_prop_n == 0); /* предложения нет — и не будет */
            CHECK(mock.sendraw_count == 0);
            b2.destroy(&b2);
        }

        /* 6l. гейт проверяет КАЖДЫЙ хоп: двухногочный маршрут, вторая
         * нога на чужой фабрике — предложение не строится, хотя
         * первая нога «своя» */
        {
            tb_exec_dex_cfg_t cfg3;
            memset(&cfg3, 0, sizeof(cfg3));
            strcpy(cfg3.venues[0].name, "uni-test");
            snprintf(cfg3.venues[0].rpc_url, sizeof(cfg3.venues[0].rpc_url),
                     "http://127.0.0.1:%u", (unsigned)mock.port);
            cfg3.venues[0].quoter =
                A("0x61fFE014bA17989E743c5F6cB21bF9697530B21e");
            cfg3.venues[0].factory =
                A("0x1F98431c8aD98523631AE4a59f267346ea31F984");
            cfg3.venues[0].pools[0] =
                A("0x88e6A0c2dDD26FEEb64F039a2c41296FcB3f5640");
            cfg3.venues[0].pools_n = 1;
            strcpy(cfg3.venues[1].name, "pancake-test");
            snprintf(cfg3.venues[1].rpc_url, sizeof(cfg3.venues[1].rpc_url),
                     "http://127.0.0.1:%u", (unsigned)mock.port);
            cfg3.venues[1].quoter =
                A("0xB048Bbc1Ee6b733FFfCFb9e9CeF7375518e25997");
            cfg3.venues[1].factory =
                A("0x0BFbCF9fa4f9C56B0F40a671Ad40E0805A091865");
            cfg3.venues[1].pools[0] =
                A("0x06da0fd433C1A5D7a4faa01111c044910A184553");
            cfg3.venues[1].pools_n = 1;
            cfg3.venues_n = 2;
            cfg3.seed = 7;
            cfg3.live = true;
            snprintf(cfg3.safe.rpc_url, sizeof(cfg3.safe.rpc_url),
                     "http://127.0.0.1:%u", (unsigned)mock.port);
            cfg3.safe.safe = SAFE;
            cfg3.safe.router = ROUTER;
            cfg3.safe.chain_id = 1;
            cfg3.approve_cap_wei = 10000000000000000ull;
            cfg3.on_propose = catch_proposal;

            tb_exec_backend_t b3;
            CHECK(tb_exec_dex_init(&b3, http, &cfg3, &err) == 0);

            tb_addr_t usdt3 = A("0xdAC17F958D2ee523a2206206994597C13D831ec7");
            tb_addr_t toks3[3] = { WETH, usdc, usdt3 };
            uint32_t fees3[2] = { 500, 3000 };
            tb_route_t r3 = tb_route_encode(toks3, fees3, 3, &err);
            tb_route_set_pool(&r3, 0, &cfg3.venues[0].pools[0]);
            tb_route_set_pool(&r3, 1, &cfg3.venues[1].pools[0]);
            CHECK(r3.len > 0);

            mock.allowance = 2000000000000000000ull;
            g_prop_n = 0;
            tb_error_t xerr3 = TB_ERROR_INIT;
            int rc3 = b3.submit(&b3, &r3, &in, &mn, 0, tb_now_ms(), &tx,
                                &got, &xerr3);
            CHECK(rc3 == -1);
            CHECK(strstr(xerr3.message, "чужой фабрике") != NULL);
            CHECK(g_prop_n == 0);
            CHECK(mock.sendraw_count == 0);
            b3.destroy(&b3);
        }

        b.destroy(&b);
    }

    tb_http_destroy(http);
    mock_stop(&mock);
    printf("OK test_safe_exec\n");
    return 0;
}
