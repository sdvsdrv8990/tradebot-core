/* ==========================================================================
 *  tests/test_net.c — HTTP-транспорт: разбор url, replay, net на локальном
 *  mock-сервере (127.0.0.1, обычный HTTP — внешних зависимостей нет).
 *
 *  HTTPS к реальным хостам здесь не тестируется (окружение-зависимо):
 *  живой сетевой прогон — arb_demo с реальным RPC.
 * ========================================================================== */
#include "tb/arch/arch.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "harness.h"
#include "tb/core/error.h"
#include "tb/core/time.h"
#include "tb/net/http.h"

/* ---- mock-сервер: раздаёт ответы по пути ---- */

typedef struct mock_srv {
    int       fd;      /* слушающий */
    int       port;
    pthread_t th;
    bool      stop;
} mock_srv_t;

static void *mock_thread(void *arg)
{
    mock_srv_t *s = arg;
    while (!tb_atomic_load_bool(&s->stop)) {
        int c = accept(s->fd, NULL, NULL);
        if (c < 0)
            break;

        char req[2048] = {0};
        size_t have = 0;
        while (have < sizeof(req) - 1 && !strstr(req, "\r\n\r\n")) {
            ssize_t n = recv(c, req + have, sizeof(req) - 1 - have, 0);
            if (n <= 0)
                break;
            have += (size_t)n;
        }
        const char *path = strchr(req, ' ');
        char route[32] = {0};
        if (path && path[1] == '/')
            for (size_t i = 1; path[i] != ' ' && path[i] != '?' && i < sizeof(route); i++)
                route[i - 1] = path[i];

        char out[4096];
        int n = 0;
        if (strcmp(route, "/notfound") == 0) {
            n = snprintf(out, sizeof(out),
                         "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
        } else if (strcmp(route, "/retry") == 0) {
            n = snprintf(out, sizeof(out),
                         "HTTP/1.1 429 Too Many\r\nRetry-After: 2\r\n"
                         "Content-Length: 0\r\n\r\n");
        } else if (strcmp(route, "/chunked") == 0) {
            n = snprintf(out, sizeof(out),
                         "HTTP/1.1 200 OK\r\n"
                         "Content-Type: application/json\r\n"
                         "Transfer-Encoding: chunked\r\n\r\n"
                         "4\r\n{\"a\"\r\n3\r\n:1}\r\n0\r\n\r\n");
        } else if (strcmp(route, "/close") == 0) {
            n = snprintf(out, sizeof(out),
                         "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n"
                         "no-length-body");
            send(c, out, (size_t)n, 0);
            close(c);
            continue;
        } else if (strcmp(route, "/slow") == 0) {
            tb_sleep_ms(400);
            n = snprintf(out, sizeof(out),
                         "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi");
        } else { /* /ok и прочее */
            const char *body = "{\"result\":\"0x1\"}";
            n = snprintf(out, sizeof(out),
                         "HTTP/1.1 200 OK\r\n"
                         "Content-Type: application/json\r\n"
                         "Content-Length: %zu\r\n\r\n%s",
                         strlen(body), body);
        }
        send(c, out, (size_t)n, 0);
        close(c);
    }
    return NULL;
}

static void mock_start(mock_srv_t *s)
{
    tb_atomic_store_bool(&s->stop, false);
    s->fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    bind(s->fd, (struct sockaddr *)&a, sizeof(a));
    listen(s->fd, 8);

    struct sockaddr_in got = {0};
    socklen_t gl = sizeof(got);
    getsockname(s->fd, (struct sockaddr *)&got, &gl);
    s->port = ntohs(got.sin_port);

    struct timeval tv = { .tv_sec = 5 };
    setsockopt(s->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    pthread_create(&s->th, NULL, mock_thread, s);
}

static void mock_stop(mock_srv_t *s)
{
    tb_atomic_store_bool(&s->stop, true);
    shutdown(s->fd, SHUT_RDWR); /* будит accept() (close — нет) */
    pthread_join(s->th, NULL);
    close(s->fd);
}

/* ---- тесты ---- */

static void test_parse_url(void)
{
    tb_error_t err = TB_ERROR_INIT;
    char host[TB_HTTP_HOST_MAX], path[TB_HTTP_URL_MAX];
    uint16_t port = 0;
    bool tls = false;

    CHECK(tb_http_parse_url("https://api.example.com/v1/x", host,
                            sizeof(host), &port, path, sizeof(path), &tls,
                            &err) == 0);
    CHECK_STR_EQ(host, "api.example.com");
    CHECK(port == 443);
    CHECK(tls);
    CHECK_STR_EQ(path, "/v1/x");

    CHECK(tb_http_parse_url("http://host:8080", host, sizeof(host), &port,
                            path, sizeof(path), &tls, &err) == 0);
    CHECK_STR_EQ(host, "host");
    CHECK(port == 8080);
    CHECK(!tls);
    CHECK_STR_EQ(path, "/");

    CHECK(tb_http_parse_url("https://h/a?b=c", host, sizeof(host), &port,
                            path, sizeof(path), &tls, &err) == 0);
    CHECK_STR_EQ(path, "/a?b=c");

    tb_error_t e2 = TB_ERROR_INIT;
    CHECK(tb_http_parse_url("ftp://x/y", host, sizeof(host), &port, path,
                            sizeof(path), &tls, &e2) != 0);
    CHECK(e2.domain == TB_DOMAIN_NET && e2.code == TB_NET_BAD_URL);
}

static void test_replay(void)
{
    tb_error_t err = TB_ERROR_INIT;
    tb_http_replay_step_t steps[] = {
        { .status = 200, .body = "{\"a\":1}" },
        { .status = 429, .retry_after_ms = 1200, .body = "" },
    };
    tb_http_t *h = tb_http_replay_create(steps, 2, &err);
    CHECK(h != NULL);
    CHECK_STR_EQ(h->name, "replay");

    tb_http_req_t req = {0};
    strcpy(req.url, "https://any/x");
    tb_http_resp_t resp;

    CHECK(tb_http_request(h, &req, &resp, &err) == 0);
    CHECK(resp.status == 200);
    CHECK(resp.body_len == 7);
    CHECK(resp.truncated == false);

    CHECK(tb_http_request(h, &req, &resp, &err) == 0);
    CHECK(resp.status == 429);
    CHECK(resp.retry_after_ms == 1200);

    /* по кругу */
    CHECK(tb_http_request(h, &req, &resp, &err) == 0);
    CHECK(resp.status == 200);

    tb_http_destroy(h);
}

static void test_net(mock_srv_t *srv)
{
    tb_error_t err = TB_ERROR_INIT;
    tb_http_t *h = tb_http_net_create("test-agent", NULL, &err);
    CHECK(h != NULL);

    char url[TB_HTTP_URL_MAX];
    tb_http_req_t req = {0};
    req.timeout_ms = 3000;
    tb_http_resp_t resp;

    /* обычный 200 с Content-Length */
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/ok", srv->port);
    strcpy(req.url, url);
    CHECK(tb_http_request(h, &req, &resp, &err) == 0);
    CHECK(resp.status == 200);
    CHECK(resp.body_len == 16);
    CHECK_STR_EQ(resp.body, "{\"result\":\"0x1\"}");
    CHECK_STR_EQ(resp.content_type, "application/json");

    /* POST с телом */
    strcpy(req.method, "POST");
    const char *body = "{\"x\":1}";
    req.body = body;
    req.body_len = strlen(body);
    CHECK(tb_http_request(h, &req, &resp, &err) == 0);
    CHECK(resp.status == 200);
    req.body = NULL;
    req.body_len = 0;
    strcpy(req.method, "GET");

    /* 404 — статус транспортом не считается ошибкой */
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/notfound", srv->port);
    strcpy(req.url, url);
    CHECK(tb_http_request(h, &req, &resp, &err) == 0);
    CHECK(resp.status == 404);

    /* Retry-After */
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/retry", srv->port);
    strcpy(req.url, url);
    CHECK(tb_http_request(h, &req, &resp, &err) == 0);
    CHECK(resp.status == 429);
    CHECK(resp.retry_after_ms == 2000);

    /* chunked */
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/chunked", srv->port);
    strcpy(req.url, url);
    CHECK(tb_http_request(h, &req, &resp, &err) == 0);
    CHECK(resp.status == 200);
    CHECK(resp.body_len == 7);
    CHECK_STR_EQ(resp.body, "{\"a\":1}");
    CHECK_STR_EQ(resp.content_type, "application/json");

    /* до закрытия, без Content-Length */
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/close", srv->port);
    strcpy(req.url, url);
    CHECK(tb_http_request(h, &req, &resp, &err) == 0);
    CHECK(resp.status == 200);
    CHECK(resp.body_len == 14);
    CHECK_STR_EQ(resp.body, "no-length-body");

    /* таймаут чтения: сервер спит 400 мс, клиент ждёт 100 */
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/slow", srv->port);
    strcpy(req.url, url);
    req.timeout_ms = 100;
    tb_error_t terr = TB_ERROR_INIT;
    CHECK(tb_http_request(h, &req, &resp, &terr) != 0);
    CHECK(terr.domain == TB_DOMAIN_NET);
    CHECK(terr.code == TB_NET_TIMEOUT);
    req.timeout_ms = 3000;

    /* соединение нет -> CONNECT */
    snprintf(url, sizeof(url), "http://127.0.0.1:9/x");
    strcpy(req.url, url);
    tb_error_t cerr = TB_ERROR_INIT;
    CHECK(tb_http_request(h, &req, &resp, &cerr) != 0);
    CHECK(cerr.domain == TB_DOMAIN_NET);
    CHECK(cerr.code == TB_NET_CONNECT);

    tb_http_destroy(h);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    test_parse_url();
    test_replay();

    mock_srv_t srv;
    mock_start(&srv);
    test_net(&srv);
    mock_stop(&srv);

    printf("OK test_net\n");
    return 0;
}
