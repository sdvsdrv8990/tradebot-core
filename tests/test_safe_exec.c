/* ==========================================================================
 *  tests/test_safe_exec.c — propose-only контур Safe на локальном
 *  mock-RPC (обычный HTTP, 127.0.0.1): calldata (exactInput/approve/
 *  execTransaction), EIP-712-хеш, ПРЯМАЯ подпись (восстановление адреса
 *  из safeTxHash), подготовка предложения БЕЗ отправки, наблюдение за
 *  nonce (исполнение человеком = расчёт по дельте баланса) и истечение
 *  окна. Ключ детерминированный — подписи воспроизводимы.
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
#include "tb/core/time.h"
#include "tb/exec/backend_dex.h"
#include "tb/exec/safe_exec.h"
#include "tb/net/http.h"
#include "tb/sign/tx.h"

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

/* ---- mock-RPC: eth_call различает nonce()/balanceOf по селектору ---- */

typedef struct mock {
    int fd, port;
    pthread_t th;
    volatile bool stop;
    int sendraw_count;      /* попыток отправки: обязано быть НУЛЁМ */
    uint64_t safe_nonce;    /* nonce() Safe; тест «исполняет» движением */
    uint64_t balance_now;   /* balanceOf до/после «исполнения» */
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

    if (strstr(body, "eth_chainId")) {
        n = snprintf(out, sizeof(out), "{\"id\":1,\"result\":\"0x1\"}");
    } else if (strstr(body, "eth_sendRawTransaction")) {
        m->sendraw_count++;
        n = snprintf(out, sizeof(out),
                     "{\"id\":1,\"error\":{\"code\":-1,"
                     "\"message\":\"propose-only: отправка запрещена\"}}");
    } else if (strstr(body, "eth_call")) {
        /* nonce() = 0x1d6da664 в calldata -> nonce Safe; остальное
         * (balanceOf 0x70a08231 / allowance) -> баланс */
        if (body && strstr(body, "1d6da664"))
            n = snprintf(out, sizeof(out),
                         "{\"id\":1,\"result\":\"0x%064llx\"}",
                         (unsigned long long)m->safe_nonce);
        else
            n = snprintf(out, sizeof(out),
                         "{\"id\":1,\"result\":\"0x%064llx\"}",
                         (unsigned long long)m->balance_now);
    } else {
        n = snprintf(out, sizeof(out), "{\"id\":1,\"error\":{\"code\":-1}}");
    }

    char hdr[128];
    int hn = snprintf(hdr, sizeof(hdr),
                      "HTTP/1.1 200 OK\r\nContent-Type: "
                      "application/json\r\nConnection: close\r\n\r\n");
    send(c, hdr, (size_t)hn, 0);
    send(c, out, (size_t)n, 0);
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
    m->stop = false; /* инициализация до pthread_create — гонки нет */
    m->sendraw_count = 0;
    m->safe_nonce = 5;
    m->balance_now = 1000;
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

/* улов предложений колбэком бэкенда */
static tb_safe_proposal_t g_prop;
static int g_prop_n;
static void catch_proposal(const tb_safe_proposal_t *p, void *user)
{
    (void)user;
    g_prop = *p;
    g_prop_n++;
}

int main(void)
{
    tb_error_t err = TB_ERROR_INIT;
    mock_t mock;
    mock_start(&mock);

    tb_http_t *http = tb_http_net_create("test", NULL, &err);
    CHECK(http != NULL);

    tb_key_t key;
    CHECK(tb_key_from_hex(
        &key,
        "0x8f2a55949038a9610f50fb23b5883af3b4ecb3c3bb792cbcefbd1542c692be63",
        &err) == 0);

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
              && buf[3] == 0x3f); /* селектор exactInput */
        /* recipient = Safe во втором слове головы (4 + 32*1) */
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
              && buf[3] == 0xb3); /* селектор approve(address,uint256) */
        CHECK(memcmp(buf + 4 + 12, ROUTER.b, 20) == 0); /* spender */
        /* amount: последнее слово */
        tb_u256_t back;
        uint64_t w[4];
        for (int i = 0; i < 4; i++) {
            w[i] = 0;
            for (int b = 0; b < 8; b++)
                w[i] = (w[i] << 8) | buf[4 + 32 + i * 8 + b];
        }
        back.w[0] = w[3];
        back.w[1] = w[2];
        back.w[2] = w[1];
        back.w[3] = w[0];
        CHECK(tb_u256_cmp(&back, &amt) == 0);
    }

    /* ---- 3. Calldata: execTransaction ---- */
    {
        uint8_t data[64] = { 0xde, 0xad, 0xbe, 0xef };
        uint8_t sig[65];
        memset(sig, 0xAA, 65);
        uint8_t buf[640];
        size_t n = tb_safe_build_exec_tx(buf, sizeof(buf), &ROUTER, 0, data,
                                         4, sig);
        CHECK(buf[0] == 0x6a && buf[1] == 0x76 && buf[2] == 0x12
              && buf[3] == 0x02); /* селектор execTransaction */
        CHECK(memcmp(buf + 4 + 12, ROUTER.b, 20) == 0); /* to */
        CHECK(n > 4 + 32 * 10);
    }

    /* ---- 4. Подпись: ПРЯМАЯ EIP-712 (Safe v=27/28 восстанавливает
     *      адрес из самого safeTxHash) ---- */
    {
        tb_safe_cfg_t cfg = {0};
        cfg.safe = SAFE;
        cfg.key = key;
        cfg.chain_id = 1;

        uint8_t data[4] = { 0xde, 0xad, 0xbe, 0xef };
        uint8_t hash[32], sig[65];
        CHECK(tb_safe_tx_hash(&cfg, &ROUTER, 0, data, 4, 7, hash, &err) == 0);
        CHECK(tb_safe_sign(&cfg, hash, sig, &err) == 0);
        CHECK(sig[64] == 27 || sig[64] == 28);

        tb_addr_t recovered;
        CHECK(tb_sig_recover_sender(hash, sig, sig + 32,
                                    sig[64] - 27, &recovered,
                                    &err) == 0);
        CHECK(tb_addr_eq(&recovered, &key.address));
    }

    /* ---- 5. Предложение: подготовка без отправки ---- */
    {
        tb_safe_cfg_t cfg = {0};
        snprintf(cfg.rpc_url, sizeof(cfg.rpc_url), "http://127.0.0.1:%u",
                 (unsigned)mock.port);
        cfg.safe = SAFE;
        cfg.key = key;

        tb_safe_proposal_t p;
        uint8_t cd[4 + 64] = {0x09, 0x5e, 0xa7, 0xb3};
        CHECK(tb_safe_propose_exec(http, &cfg, &WETH, 0, cd, sizeof(cd),
                                   tb_now_ms(), &p, &err) == 0);
        CHECK(p.chain_id == 1);        /* eth_chainId с mock */
        CHECK(p.safe_nonce == 5);      /* nonce() с mock */
        CHECK(p.expires_at_ms > p.created_ms);
        CHECK(p.data_len == sizeof(cd));
        CHECK(memcmp(p.data, cd, sizeof(cd)) == 0);

        /* подпись предложения — тоже прямая EIP-712 */
        tb_addr_t recovered;
        CHECK(tb_sig_recover_sender(p.safe_tx_hash, p.sig65, p.sig65 + 32,
                                    p.sig65[64] - 27, &recovered,
                                    &err) == 0);
        CHECK(tb_addr_eq(&recovered, &key.address));

        /* JSON-артефакт содержит ключевые поля */
        char js[1600];
        size_t jn = tb_safe_proposal_json(&p, js, sizeof(js));
        CHECK(jn > 100);
        CHECK(strstr(js, "\"safe-proposal\"") != NULL);
        CHECK(strstr(js, "\"nonce\":5") != NULL);
        CHECK(strstr(js, "\"bot_signature\":\"0x") != NULL);

        /* exec-calldata предложения — валидный execTransaction */
        uint8_t buf[1024];
        size_t en = tb_safe_proposal_exec_calldata(&p, buf, sizeof(buf));
        CHECK(en > 4 + 32 * 10);
        CHECK(buf[0] == 0x6a && buf[1] == 0x76);
    }

    /* ---- 6. Полный live-цикл через DEX-адаптер: propose -> наблюдение
     *      -> «исполнение человеком» -> дельта баланса ---- */
    {
        tb_exec_dex_cfg_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        strcpy(cfg.venues[0].name, "uni-test");
        snprintf(cfg.venues[0].rpc_url, sizeof(cfg.venues[0].rpc_url),
                 "http://127.0.0.1:%u", (unsigned)mock.port);
        cfg.venues[0].quoter = A("0x61fFE014bA17989E743c5F6cB21bF9697530B21e");
        cfg.venues[0].pools[0] = A("0x88e6A0c2dDD26FEEb64F039a2c41296FcB3f5640");
        cfg.venues[0].pools_n = 1;
        cfg.venues_n = 1;
        cfg.seed = 42;
        cfg.live = true;
        cfg.safe.key = key;
        snprintf(cfg.safe.rpc_url, sizeof(cfg.safe.rpc_url),
                 "http://127.0.0.1:%u", (unsigned)mock.port);
        cfg.safe.safe = SAFE;
        cfg.safe.router = ROUTER;
        cfg.safe.chain_id = 1;
        cfg.on_propose = catch_proposal;

        tb_exec_backend_t b;
        CHECK(tb_exec_dex_init(&b, http, &cfg, &err) == 0);

        tb_addr_t usdc = A("0xA0b86991c6218b36c1d19D4a2e9Eb0cE3606eB48");
        tb_route_t r = tb_route_single(&WETH, &usdc, 500, &err);
        tb_route_set_pool(&r, 0, &cfg.venues[0].pools[0]);

        /* submit: предложить (никакой отправки!) */
        tb_u256_t in, mn, got;
        tb_u256_from_dec(&in, "1000000000000000", 16, NULL);
        tb_u256_from_dec(&mn, "900", 3, NULL); /* ниже mock-котировки */
        uint64_t tx = 0;
        tb_error_t serr = TB_ERROR_INIT;
        int rc = b.submit(&b, &r, &in, &mn, 0, tb_now_ms(), &tx, &got,
                          &serr);
        if (rc != 1)
            printf("submit rc=%d err=%s\n", rc, serr.message);
        CHECK(rc == 1);
        CHECK(tx != 0);
        CHECK(mock.sendraw_count == 0); /* НИЧЕГО не отправлено */
        CHECK(g_prop_n == 1);           /* предложение отдано наверх */
        CHECK(g_prop.safe_nonce == 5);

        /* poll: nonce не двигался — ждём */
        tb_error_t perr = TB_ERROR_INIT;
        CHECK(b.poll(&b, tx, tb_now_ms(), &got, &perr) == 1);

        /* «человек исполнил»: nonce вырос, баланс изменился */
        mock.safe_nonce = 6;
        mock.balance_now = 1500;
        CHECK(b.poll(&b, tx, tb_now_ms(), &got, &perr) == 0);
        CHECK(got.w[0] == 500); /* дельта 1500-1000 */
        CHECK(got.w[1] == 0);

        /* ---- 7. Истечение окна: отказ без цепи ---- */
        g_prop_n = 0;
        rc = b.submit(&b, &r, &in, &mn, 0, tb_now_ms(), &tx, &got, &serr);
        CHECK(rc == 1);
        CHECK(g_prop_n == 1);
        CHECK(mock.sendraw_count == 0);
        /* далеко за окном наблюдения */
        tb_error_t eerr = TB_ERROR_INIT;
        CHECK(b.poll(&b, tx, tb_now_ms() + TB_SAFE_PROPOSAL_WATCH_MS + 1000,
                     &got, &eerr) == -1);
        CHECK(strstr(eerr.message, "истекло") != NULL);
        CHECK(mock.sendraw_count == 0); /* и после истечения — тишина */

        b.destroy(&b);
    }

    tb_http_destroy(http);
    mock_stop(&mock);
    printf("OK test_safe_exec\n");
    return 0;
}
