/* ==========================================================================
 *  tests/test_safe_exec.c — живое исполнение из Safe на локальном
 *  mock-RPC (обычный HTTP, 127.0.0.1): полный цикл калибровки calldata,
 *  подписи EIP-712, отправки, receipt и дельты баланса. Ключ
 *  детерминированный — все подписи воспроизводимы.
 * ==========================================================================
 */
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
#include "tb/exec/backend_dex.h"
#include "tb/exec/safe_exec.h"
#include "tb/net/http.h"
#include "tb/core/time.h"

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

/* ---- mock-RPC ---- */

typedef struct mock {
    int fd, port;
    pthread_t th;
    volatile bool stop;
    int sent_count;          /* sendRawTransaction вызовов */
    int receipts_served;     /* сколько раз отдали receipt */
    uint64_t balance_after;  /* что отдаёт balanceOf после «майнинга» */
} mock_t;

/* разбор параметров: method из тела */
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
    } else if (strstr(body, "eth_gasPrice")) {
        n = snprintf(out, sizeof(out), "{\"id\":1,\"result\":\"0x%x\"}",
                     20000000000u);
    } else if (strstr(body, "eth_maxPriorityFeePerGas")) {
        n = snprintf(out, sizeof(out), "{\"id\":1,\"result\":\"0x%x\"}",
                     1000000000u);
    } else if (strstr(body, "eth_estimateGas")) {
        n = snprintf(out, sizeof(out), "{\"id\":1,\"result\":\"0x%x\"}",
                     300000u);
    } else if (strstr(body, "eth_getTransactionCount")) {
        n = snprintf(out, sizeof(out), "{\"id\":1,\"result\":\"0x7\"}");
    } else if (strstr(body, "eth_sendRawTransaction")) {
        m->sent_count++;
        /* проверка: raw начинается с 0x02 (EIP-1559) */
        const char *r = strstr(body, "\"0x");
        CHECK(r != NULL);
        CHECK(strncmp(r + 1, "0x02", 4) == 0);
        n = snprintf(out, sizeof(out),
                     "{\"id\":1,\"result\":\"""0x1111111111111111111111111111111111111111111111111111111111111111""}");
    } else if (strstr(body, "eth_getTransactionReceipt")) {
        if (m->receipts_served++ == 0)
            n = snprintf(out, sizeof(out), "{\"id\":1,\"result\":null}");
        else
            n = snprintf(out, sizeof(out),
                         "{\"id\":1,\"result\":{\"status\":\"0x1\"}}");
    } else if (strstr(body, "eth_call")) {
        /* balanceOf/allowance/nonce: без разбору — растущий баланс,
         * чтобы дельта была видна после «майнинга» */
        uint64_t bal = m->receipts_served > 1 ? m->balance_after : 1000;
        n = snprintf(out, sizeof(out),
                     "{\"id\":1,\"result\":\"0x%064llx\"}",
                     (unsigned long long)bal);
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
    m->sent_count = 0;
    m->receipts_served = 0;
    m->balance_after = 1500;
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
    struct timeval tv; tv.tv_sec = 5; tv.tv_usec = 0;
    setsockopt(m->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    pthread_create(&m->th, NULL, mock_thread, m);
}

static void mock_stop(mock_t *m)
{
    tb_atomic_store_bool(&m->stop, true);
    shutdown(m->fd, SHUT_RDWR); /* будит accept() (close — нет) */
    pthread_join(m->th, NULL);
    close(m->fd);
}

static void fill_cfg(tb_exec_dex_cfg_t *cfg, mock_t *m)
{
    memset(cfg, 0, sizeof(*cfg));
    strcpy(cfg->venues[0].name, "uni-test");
    snprintf(cfg->venues[0].rpc_url, sizeof(cfg->venues[0].rpc_url),
             "http://127.0.0.1:%u", (unsigned)m->port);
    cfg->venues[0].quoter = A("0x61fFE014bA17989E743c5F6cB21bF9697530B21e");
    cfg->venues[0].pools[0] = A("0x88e6A0c2dDD26FEEb64F039a2c41296FcB3f5640");
    cfg->venues[0].pools_n = 1;
    cfg->venues_n = 1;
    cfg->seed = 42;
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
              && buf[3] == 0x3f); /* селектор exactInput */
        /* recipient = Safe во втором слове головы (4 + 32*1) */
        CHECK(memcmp(buf + 4 + 32 * 1 + 12, SAFE.b, 20) == 0);
    }

    /* ---- 2. Calldata: execTransaction ---- */
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

    /* ---- 3. Полный live-цикл через DEX-адаптер ---- */
    {
        tb_exec_dex_cfg_t cfg;
        fill_cfg(&cfg, &mock);
        cfg.live = true;
        CHECK(tb_key_from_hex(
            &cfg.safe.key,
            "0x8f2a55949038a9610f50fb23b5883af3b4ecb3c3bb792cbcefbd1542c692be63",
            &err) == 0);
        snprintf(cfg.safe.rpc_url, sizeof(cfg.safe.rpc_url),
                 "http://127.0.0.1:%u", (unsigned)mock.port);
        cfg.safe.safe = SAFE;
        cfg.safe.router = ROUTER;
        cfg.safe.chain_id = 1;

        tb_exec_backend_t b;
        CHECK(tb_exec_dex_init(&b, http, &cfg, &err) == 0);

        /* котировка (replay не нужна: mock отвечает на eth_call
         * фиксированным значением) */
        tb_addr_t usdc = A("0xA0b86991c6218b36c1d19D4a2e9Eb0cE3606eB48");
        tb_route_t r = tb_route_single(&WETH, &usdc, 500, &err);
        tb_route_set_pool(&r, 0, &cfg.venues[0].pools[0]);
        tb_u256_t out;
        tb_u256_t q_in = tb_u256_from_u64(1000);
        CHECK(b.quote(&b, &r, &q_in, 0, &out, &err) == 0);

        /* submit: живой путь — подпись, отправка, session */
        tb_u256_t in, mn;
        tb_u256_from_dec(&in, "1000000000000000", 16, NULL);
        tb_u256_from_dec(&mn, "900", 3, NULL); /* ниже mock-котировки */
        uint64_t tx = 0;
        tb_u256_t got;
        tb_error_t serr = TB_ERROR_INIT;
        int rc = b.submit(&b, &r, &in, &mn, 0, tb_now_ms(), &tx, &got,
                          &serr);
        if (rc != 1)
            printf("submit rc=%d err=%s\n", rc, serr.message);
        CHECK(rc == 1);
        CHECK(tx != 0);
        CHECK(mock.sent_count == 1); /* транзакция реально отправлена */

        /* poll 1: receipt null — pending */
        tb_error_t perr = TB_ERROR_INIT;
        CHECK(b.poll(&b, tx, tb_now_ms(), &got, &perr) == 1);

        /* poll 2: receipt 0x1, дельта баланса 1500-1000 */
        CHECK(b.poll(&b, tx, tb_now_ms(), &got, &perr) == 0);
        CHECK(got.w[0] == 500);
        CHECK(got.w[1] == 0);

        b.destroy(&b);
    }

    tb_http_destroy(http);
    mock_stop(&mock);
    printf("OK test_safe_exec\n");
    return 0;
}
