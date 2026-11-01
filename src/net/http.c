/* ==========================================================================
 *  tb/net/http.c — HTTP-транспорт: net (сокеты + TLS) и replay.
 *
 *  net: один запрос — одно соединение (без keep-alive, v0.3). connect —
 *  неблокирующий с poll-таймаутом, далее сокет блокирующий с SO_RCVTIMEO/
 *  SO_SNDTIMEO. TLS — mbedTLS (вендор): контексты на запрос, корневые
 *  сертификаты разобраны один раз и дальше только читаются.
 *
 *  Ответ: заголовки в фиксированный буфер, тело — в tb_http_resp_t
 *  (усечение помечается, не является ошибкой). Поддержаны Content-Length,
 *  Transfer-Encoding: chunked и чтение до закрытия.
 * ========================================================================== */
#include "tb/net/http.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "tb/core/time.h"

#define NET_FAIL(code, ...)                                     \
    do {                                                        \
        TB_ERROR_SET(err, TB_DOMAIN_NET, (code), __VA_ARGS__);  \
        return -1;                                              \
    } while (0)

static void ms_to_timeval(uint32_t ms, struct timeval *tv)
{
    tv->tv_sec = ms / 1000;
    tv->tv_usec = (suseconds_t)(ms % 1000) * 1000;
}

/* ==========================================================================
 *  Разбор url
 * ========================================================================== */

int tb_http_parse_url(const char *url, char *host, size_t host_cap,
                      uint16_t *port, char *path, size_t path_cap,
                      bool *is_tls, tb_error_t *err)
{
    if (!url || !host || !port || !path || !is_tls || !*url)
        NET_FAIL(TB_NET_BAD_URL, "пустой url");

    const char *p = url;
    bool tls = true;

    if (strncmp(p, "https://", 8) == 0) {
        p += 8;
    } else if (strncmp(p, "http://", 7) == 0) {
        p += 7;
        tls = false;
    } else {
        NET_FAIL(TB_NET_BAD_URL, "нет схемы http(s)://: '%.32s'", url);
    }

    const char *slash = strchr(p, '/');
    size_t hlen = slash ? (size_t)(slash - p) : strlen(p);
    const char *colon = memchr(p, ':', hlen);
    if (colon)
        hlen = (size_t)(colon - p);

    if (hlen == 0 || hlen >= host_cap)
        NET_FAIL(TB_NET_BAD_URL, "хост пуст или длиннее %zu: '%.32s'",
                 host_cap, p);

    memcpy(host, p, hlen);
    host[hlen] = '\0';

    uint16_t port_v = 0;
    if (colon) {
        char *end = NULL;
        unsigned long v = strtoul(colon + 1, &end, 10);
        if (end == colon + 1 || v == 0 || v > 65535)
            NET_FAIL(TB_NET_BAD_URL, "неверный порт: '%.16s'", colon + 1);
        port_v = (uint16_t)v;
    }
    if (port_v == 0)
        port_v = tls ? 443 : 80;

    const char *pp = slash ? slash : "/";
    if (strlen(pp) >= path_cap)
        NET_FAIL(TB_NET_BAD_URL, "путь длиннее %zu", path_cap - 1);
    strcpy(path, pp);

    *port = port_v;
    *is_tls = tls;
    return 0;
}

/* ==========================================================================
 *  Разбор заголовков ответа (чистая функция)
 *  Возврат: content_length >= 0 | -1 chunked | -2 до закрытия | -3 не HTTP.
 * ========================================================================== */

static int64_t parse_headers(const char *hdr, tb_http_resp_t *resp)
{
    if (strncmp(hdr, "HTTP/1.", 7) != 0 || !isdigit((unsigned char)hdr[7])
        || hdr[8] != ' ')
        return -3;
    resp->status = (int32_t)strtol(hdr + 9, NULL, 10);
    if (resp->status < 100 || resp->status > 599)
        return -3;

    int64_t content_length = -2;
    bool chunked = false;

    const char *line = strstr(hdr, "\r\n");
    while (line) {
        line += 2;
        const char *eol = strstr(line, "\r\n");
        size_t len = eol ? (size_t)(eol - line) : strlen(line);
        if (len == 0)
            break;

        if (len > 15 && strncasecmp(line, "Content-Length:", 15) == 0)
            content_length = (int64_t)strtoll(line + 15, NULL, 10);
        else if (len > 18 && strncasecmp(line, "Transfer-Encoding:", 18) == 0) {
            const char *v = line + 18;
            while (*v == ' ' || *v == '\t')
                v++;
            if (strncasecmp(v, "chunked", 7) == 0)
                chunked = true;
        } else if (len > 12 && strncasecmp(line, "Retry-After:", 12) == 0) {
            const char *v = line + 12;
            while (*v == ' ')
                v++;
            if (isdigit((unsigned char)*v))
                resp->retry_after_ms = (uint32_t)(strtoul(v, NULL, 10) * 1000);
            /* HTTP-даты не поддержаны — останется 0 */
        } else if (len > 13 && strncasecmp(line, "Content-Type:", 13) == 0) {
            const char *v = line + 13;
            while (*v == ' ')
                v++;
            size_t n = 0;
            while (v[n] && v[n] != ';' && v[n] != '\r'
                   && n + 1 < TB_HTTP_CTYPE_MAX)
                n++;
            memcpy(resp->content_type, v, n);
            resp->content_type[n] = '\0';
        }
        line = eol;
    }
    if (chunked)
        return -1;
    return content_length;
}

/* ==========================================================================
 *  Реализация replay
 * ========================================================================== */

typedef struct replay_ctx {
    tb_http_replay_step_t *steps;
    size_t                 n;
    size_t                 pos;
    pthread_mutex_t        lock;
} replay_ctx_t;

static void replay_destroy(tb_http_t *h)
{
    replay_ctx_t *c = h->ctx;
    if (c) {
        pthread_mutex_destroy(&c->lock);
        for (size_t i = 0; i < c->n; i++)
            free((void *)c->steps[i].body);
        free(c->steps);
        free(c);
    }
    free(h);
}

static int replay_request(tb_http_t *h, const tb_http_req_t *req,
                          tb_http_resp_t *resp, tb_error_t *err)
{
    (void)req;
    (void)err;
    replay_ctx_t *c = h->ctx;

    pthread_mutex_lock(&c->lock);
    size_t i = c->pos++ % c->n;
    pthread_mutex_unlock(&c->lock);

    const tb_http_replay_step_t *s = &c->steps[i];
    if (s->latency_ms)
        tb_sleep_ms(s->latency_ms);

    memset(resp, 0, sizeof(*resp));
    resp->status = s->status;
    resp->retry_after_ms = s->retry_after_ms;
    size_t len = s->body ? strlen(s->body) : 0;
    if (len > TB_HTTP_BODY_MAX) {
        len = TB_HTTP_BODY_MAX;
        resp->truncated = true;
    }
    if (len) {
        memcpy(resp->body, s->body, len);
        resp->body_len = len;
    }
    return 0;
}

tb_http_t *tb_http_replay_create(const tb_http_replay_step_t *steps,
                                 size_t n, tb_error_t *err)
{
    if (!steps || n == 0) {
        TB_ERROR_SET(err, TB_DOMAIN_NET, TB_NET_RESPONSE,
                     "replay: нет сценариев");
        return NULL;
    }

    tb_http_t *h = calloc(1, sizeof(*h));
    replay_ctx_t *c = calloc(1, sizeof(*c));
    if (h && c)
        c->steps = calloc(n, sizeof(*c->steps));
    if (!h || !c || !c->steps)
        goto oom;

    for (size_t i = 0; i < n; i++) {
        c->steps[i] = steps[i];
        c->steps[i].body = NULL;
        if (steps[i].body) {
            size_t len = strlen(steps[i].body);
            c->steps[i].body = malloc(len + 1);
            if (!c->steps[i].body)
                goto oom;
            memcpy((void *)c->steps[i].body, steps[i].body, len + 1);
        }
    }
    c->n = n;
    pthread_mutex_init(&c->lock, NULL);

    h->name = "replay";
    h->ctx = c;
    h->destroy = replay_destroy;
    h->request = replay_request;
    return h;

oom:
    if (c)
        for (size_t i = 0; i < n; i++)
            free((void *)c->steps[i].body);
    if (c)
        free(c->steps);
    free(c);
    free(h);
    TB_ERROR_SET(err, TB_DOMAIN_NET, TB_NET_RESPONSE, "replay: нехватка памяти");
    return NULL;
}

/* ==========================================================================
 *  Реализация net (сокеты + TLS)
 * ========================================================================== */

typedef struct net_ctx {
    char            *ua;
    mbedtls_x509_crt ca;   /* корневые сертификаты: разбор один раз,
                              дальше только чтение из воркеров пула */
    bool             ca_ok;
} net_ctx_t;

static void net_free_ctx(net_ctx_t *c)
{
    if (!c)
        return;
    if (c->ca_ok)
        mbedtls_x509_crt_free(&c->ca);
    free(c->ua);
    free(c);
}

static void net_destroy(tb_http_t *h)
{
    net_free_ctx(h->ctx);
    free(h);
}

static const char *const tb_ca_candidates[] = {
    "/etc/pki/tls/certs/ca-bundle.crt",   /* Fedora/RHEL */
    "/etc/ssl/certs/ca-certificates.crt", /* Debian/Ubuntu */
    NULL
};

static int net_load_ca(net_ctx_t *c, const char *ca_file, tb_error_t *err)
{
    const char *path = ca_file;
    if (!path)
        path = getenv("TB_CA_FILE");
    if (!path)
        for (int i = 0; tb_ca_candidates[i] && !path; i++)
            if (access(tb_ca_candidates[i], R_OK) == 0)
                path = tb_ca_candidates[i];
    if (!path) {
        TB_ERROR_SET(err, TB_DOMAIN_NET, TB_NET_TLS,
                     "не найден файл корневых сертификатов "
                     "(задайте TB_CA_FILE или ca_file)");
        return -1;
    }
    int rc = mbedtls_x509_crt_parse_file(&c->ca, path);
    if (rc != 0) {
        TB_ERROR_SET(err, TB_DOMAIN_NET, TB_NET_TLS,
                     "не разобран CA-файл '%s' (mbedtls rc=-0x%04x)", path,
                     -rc);
        return -1;
    }
    c->ca_ok = true;
    return 0;
}

/* ---- соединение с таймаутом ---- */

static int dial(const char *host, uint16_t port, uint32_t timeout_ms,
                int *fd_out, tb_error_t *err)
{
    char port_s[8];
    snprintf(port_s, sizeof(port_s), "%u", (unsigned)port);

    struct addrinfo hints = {0};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, port_s, &hints, &res) != 0 || !res)
        NET_FAIL(TB_NET_DNS, "резолв %s:%u не удался", host, (unsigned)port);

    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;

        int fl = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc != 0 && errno == EINPROGRESS) {
            struct pollfd pfd = { .fd = fd, .events = POLLOUT };
            rc = poll(&pfd, 1, (int)timeout_ms);
            if (rc > 0 && (pfd.revents & POLLOUT)) {
                /* POLLOUT бывает и при ошибке — истину говорит SO_ERROR */
                int soerr = 0;
                socklen_t sl = sizeof(soerr);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) == 0
                    && soerr == 0)
                    rc = 0;
                else
                    rc = -1;
            } else {
                rc = -1;
            }
        }
        if (rc != 0) {
            close(fd);
            fd = -1;
            continue;
        }
        fcntl(fd, F_SETFL, fl);
        break;
    }
    freeaddrinfo(res);
    if (fd < 0)
        NET_FAIL(TB_NET_CONNECT, "соединение с %s:%u не установлено", host,
                 (unsigned)port);

    *fd_out = fd;
    return 0;
}

/* ---- источник чтения: plain fd или TLS ---- */

typedef int (*src_read_fn)(void *io, char *buf, size_t cap);

static int read_plain(void *io, char *buf, size_t cap)
{
    int n = (int)recv(*(int *)io, buf, cap, 0);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return -EAGAIN; /* таймаут SO_RCVTIMEO */
    return n;
}

static int read_tls(void *io, char *buf, size_t cap)
{
    int rc = mbedtls_ssl_read((mbedtls_ssl_context *)io,
                              (unsigned char *)buf, cap);
    if (rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
        return 0;
    if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE)
        return -EAGAIN;
    /* Блокирующий сокет + SO_RCVTIMEO: mbedTLS не отличает таймаут от
     * ошибки — различаем по errno. */
    if (rc < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return -EAGAIN;
    return rc;
}

/* ---- буферизованный ридер поверх источника ---- */

typedef struct br {
    src_read_fn rd;
    void       *io;
    tb_error_t *err;
    const char *pend;   /* непотреблённый хвост после заголовков */
    size_t      pend_len;
} br_t;

/* Ровно len байт (len <= cap буфера вызывающего). 0 — успех. */
static int br_read_exact(br_t *b, char *buf, size_t len)
{
    size_t done = 0;
    while (done < len) {
        if (b->pend_len == 0) {
            int n = b->rd(b->io, buf + done, len - done);
            if (n == 0)
                TB_ERROR_SET(b->err, TB_DOMAIN_NET, TB_NET_CLOSED,
                             "соединение закрыто до полного ответа");
            else if (n == -EAGAIN)
                TB_ERROR_SET(b->err, TB_DOMAIN_NET, TB_NET_TIMEOUT,
                             "таймаут чтения ответа");
            else if (n < 0)
                TB_ERROR_SET(b->err, TB_DOMAIN_NET, TB_NET_CLOSED,
                             "чтение ответа: rc=%d", n);
            if (n <= 0)
                return -1;
            done += (size_t)n;
            continue;
        }
        size_t take = b->pend_len;
        if (take > len - done)
            take = len - done;
        memcpy(buf + done, b->pend, take);
        b->pend += take;
        b->pend_len -= take;
        done += take;
    }
    return 0;
}

static int br_read_byte(br_t *b, char *out)
{
    return br_read_exact(b, out, 1);
}

/* Добавить data к телу с усечением по буферу. */
static void body_append(tb_http_resp_t *resp, const char *data, size_t len)
{
    if (resp->body_len >= TB_HTTP_BODY_MAX) {
        resp->truncated = true;
        return;
    }
    size_t room = TB_HTTP_BODY_MAX - resp->body_len;
    if (len > room) {
        memcpy(resp->body + resp->body_len, data, room);
        resp->truncated = true;
    } else {
        memcpy(resp->body + resp->body_len, data, len);
    }
    resp->body_len += len;
}

/* ---- заголовки: до CRLFCRLF; хвост сохраняется в ридер ---- */

#define HDR_BUF_MAX 8192

static int read_headers(br_t *b, char *hdr, size_t cap, tb_error_t *err)
{
    size_t have = 0;
    hdr[0] = '\0';
    for (;;) {
        if (have + 2 >= cap)
            NET_FAIL(TB_NET_RESPONSE, "заголовки больше %zu байт", cap);
        int n = b->rd(b->io, hdr + have, cap - have - 1);
        if (n == 0)
            NET_FAIL(TB_NET_CLOSED, "соединение закрыто до заголовков");
        if (n < 0) {
            if (n == -EAGAIN)
                NET_FAIL(TB_NET_TIMEOUT, "таймаут чтения заголовков");
            NET_FAIL(TB_NET_CLOSED, "чтение заголовков: rc=%d", n);
        }
        have += (size_t)n;
        hdr[have] = '\0';
        char *sep = strstr(hdr, "\r\n\r\n");
        if (sep) {
            size_t hdr_len = (size_t)(sep + 4 - hdr);
            b->pend = sep + 4;
            b->pend_len = have - hdr_len;
            sep[2] = '\0'; /* заголовки заканчиваются последним CRLF */
            return 0;
        }
    }
}

/* ---- тело ---- */

static int read_body(br_t *b, int64_t how, tb_http_resp_t *resp,
                     tb_error_t *err)
{
    char tmp[8192];

    if (how >= 0) {
        /* Content-Length */
        while ((int64_t)resp->body_len < how) {
            size_t want = sizeof(tmp);
            if (want > (size_t)((int64_t)how - (int64_t)resp->body_len))
                want = (size_t)((int64_t)how - (int64_t)resp->body_len);
            if (b->pend_len >= want) {
                body_append(resp, b->pend, want);
                b->pend += want;
                b->pend_len -= want;
                continue;
            }
            if (br_read_exact(b, tmp, want) != 0)
                return -1;
            body_append(resp, tmp, want);
        }
        return 0;
    }

    if (how == -1) {
        /* chunked: строка hex-размера, данные, CRLF; 0 — конец */
        for (;;) {
            char line[32];
            size_t len = 0;
            for (;;) {
                char ch;
                if (br_read_byte(b, &ch) != 0)
                    return -1;
                if (ch == '\n')
                    break;
                if (len + 1 >= sizeof(line))
                    NET_FAIL(TB_NET_RESPONSE, "chunked: строка размера велика");
                if (ch != '\r')
                    line[len++] = ch;
            }
            line[len] = '\0';
            int64_t size = (int64_t)strtoll(line, NULL, 16);
            if (size == 0)
                return 0; /* конечный чанк (трейлеры не читаем) */
            while (size > 0) {
                size_t want = sizeof(tmp);
                if (want > (size_t)size)
                    want = (size_t)size;
                if (br_read_exact(b, tmp, want) != 0)
                    return -1;
                body_append(resp, tmp, want);
                size -= (int64_t)want;
            }
            char crlf[2];
            if (br_read_exact(b, crlf, 2) != 0)
                return -1; /* CRLF после данных чанка */
        }
    }

    /* до закрытия */
    for (;;) {
        if (b->pend_len > 0) {
            body_append(resp, b->pend, b->pend_len);
            b->pend_len = 0;
            continue;
        }
        int n = b->rd(b->io, tmp, sizeof(tmp));
        if (n == 0)
            return 0;
        if (n < 0) {
            if (n == -EAGAIN)
                NET_FAIL(TB_NET_TIMEOUT, "таймаут чтения (до закрытия)");
            NET_FAIL(TB_NET_CLOSED, "чтение тела: rc=%d", n);
        }
        body_append(resp, tmp, (size_t)n);
    }
}

/* ---- запрос ---- */

static int net_request(tb_http_t *h, const tb_http_req_t *req,
                       tb_http_resp_t *resp, tb_error_t *err)
{
    net_ctx_t *c = h->ctx;
    char host[TB_HTTP_HOST_MAX], path[TB_HTTP_URL_MAX];
    uint16_t port = 0;
    bool is_tls = true;

    if (tb_http_parse_url(req->url, host, sizeof(host), &port, path,
                          sizeof(path), &is_tls, err) != 0)
        return -1;
    if (is_tls && !c->ca_ok)
        NET_FAIL(TB_NET_TLS, "нет корневых сертификатов для https");

    uint32_t timeout_ms = req->timeout_ms ? req->timeout_ms : 10000;
    const char *connect_host = req->host_pinned[0] ? req->host_pinned : host;

    int fd = -1;
    if (dial(connect_host, port, timeout_ms, &fd, err) != 0)
        return -1;

    struct timeval tv;
    ms_to_timeval(timeout_ms, &tv);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    /* Host: с портом, если он нестандартный */
    char host_hdr[TB_HTTP_HOST_MAX + 8];
    bool std_port = (is_tls && port == 443) || (!is_tls && port == 80);
    if (std_port)
        snprintf(host_hdr, sizeof(host_hdr), "%s", host);
    else
        snprintf(host_hdr, sizeof(host_hdr), "%s:%u", host, (unsigned)port);

    const char *method = req->method[0] ? req->method : "GET";
    const char *ctype = req->content_type ? req->content_type
                                          : "application/json";
    char out[8192];
    int n = snprintf(out, sizeof(out),
                     "%s %s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "User-Agent: %s\r\n"
                     "Accept: */*\r\n"
                     "Connection: close\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "\r\n",
                     method, path, host_hdr, c->ua, ctype,
                     req->body ? req->body_len : 0);
    if (n < 0 || (size_t)n >= sizeof(out) - req->body_len)
        NET_FAIL(TB_NET_RESPONSE, "запрос не влез в буфер");
    if (req->body && req->body_len)
        memcpy(out + n, req->body, req->body_len);
    size_t out_len = (size_t)n + (req->body ? req->body_len : 0);

    int rc = -1;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    bool tls_up = false;

    if (is_tls) {
        mbedtls_ssl_init(&ssl);
        mbedtls_ssl_config_init(&conf);
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&ctr_drbg);
        tls_up = true;

        if (mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                  NULL, 0) != 0) {
            TB_ERROR_SET(err, TB_DOMAIN_NET, TB_NET_TLS, "нет энтропии");
            goto out;
        }
        if (mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT,
                                        MBEDTLS_SSL_TRANSPORT_STREAM,
                                        MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
            TB_ERROR_SET(err, TB_DOMAIN_NET, TB_NET_TLS, "конфиг TLS");
            goto out;
        }
        mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&conf, &c->ca, NULL);
        mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &ctr_drbg);

        if (mbedtls_ssl_setup(&ssl, &conf) != 0
            || mbedtls_ssl_set_hostname(&ssl, host) != 0) {
            TB_ERROR_SET(err, TB_DOMAIN_NET, TB_NET_TLS, "setup TLS");
            goto out;
        }
        mbedtls_net_context net = { .fd = fd };
        mbedtls_ssl_set_bio(&ssl, &net, mbedtls_net_send, mbedtls_net_recv,
                            NULL);

        int r;
        while ((r = mbedtls_ssl_handshake(&ssl)) != 0) {
            if (r == MBEDTLS_ERR_SSL_WANT_READ
                || r == MBEDTLS_ERR_SSL_WANT_WRITE)
                continue;
            if ((r == MBEDTLS_ERR_NET_RECV_FAILED
                 || r == MBEDTLS_ERR_NET_SEND_FAILED)
                && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                TB_ERROR_SET(err, TB_DOMAIN_NET, TB_NET_TIMEOUT,
                             "таймаут TLS-рукопожатия с %s", host);
                goto out;
            }
            TB_ERROR_SET(err, TB_DOMAIN_NET, TB_NET_TLS,
                         "TLS-рукопожатие с %s (rc=-0x%04x)", host, -r);
            goto out;
        }
        size_t done = 0;
        while (done < out_len) {
            r = mbedtls_ssl_write(&ssl, (const unsigned char *)out + done,
                                  out_len - done);
            if (r == MBEDTLS_ERR_SSL_WANT_READ
                || r == MBEDTLS_ERR_SSL_WANT_WRITE)
                continue;
            if (r <= 0) {
                if ((r == MBEDTLS_ERR_NET_RECV_FAILED
                     || r == MBEDTLS_ERR_NET_SEND_FAILED)
                    && (errno == EAGAIN || errno == EWOULDBLOCK))
                    TB_ERROR_SET(err, TB_DOMAIN_NET, TB_NET_TIMEOUT,
                                 "таймаут TLS-записи");
                else
                    TB_ERROR_SET(err, TB_DOMAIN_NET, TB_NET_CLOSED,
                                 "TLS-запись: rc=-0x%04x", -r);
                goto out;
            }
            done += (size_t)r;
        }
    } else {
        size_t done = 0;
        while (done < out_len) {
            ssize_t w = send(fd, out + done, out_len - done, MSG_NOSIGNAL);
            if (w > 0) {
                done += (size_t)w;
                continue;
            }
            if (w < 0 && errno == EINTR)
                continue;
            if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                TB_ERROR_SET(err, TB_DOMAIN_NET, TB_NET_TIMEOUT,
                             "таймаут записи запроса");
            else
                TB_ERROR_SET(err, TB_DOMAIN_NET, TB_NET_CLOSED,
                             "запись запроса: errno=%d", errno);
            goto out;
        }
    }

    /* ---- ответ ---- */
    char hdr[HDR_BUF_MAX];
    memset(resp, 0, sizeof(*resp));

    br_t br = {
        .rd = is_tls ? read_tls : read_plain,
        .io = is_tls ? (void *)&ssl : (void *)&fd,
        .err = err,
    };

    if (read_headers(&br, hdr, sizeof(hdr), err) != 0)
        goto out;
    /* pend/pend_len выставлены read_headers */
    br.err = err;

    int64_t how = parse_headers(hdr, resp);
    if (how == -3) {
        TB_ERROR_SET(err, TB_DOMAIN_NET, TB_NET_RESPONSE,
                     "ответ не HTTP: '%.16s'", hdr);
        goto out;
    }
    if (read_body(&br, how, resp, err) != 0)
        goto out;

    rc = 0;

out:
    if (tls_up) {
        mbedtls_ssl_close_notify(&ssl);
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&conf);
        mbedtls_ctr_drbg_free(&ctr_drbg);
        mbedtls_entropy_free(&entropy);
    }
    close(fd);
    return rc;
}

tb_http_t *tb_http_net_create(const char *user_agent, const char *ca_file,
                              tb_error_t *err)
{
    tb_http_t *h = calloc(1, sizeof(*h));
    net_ctx_t *c = calloc(1, sizeof(*c));
    if (!h || !c) {
        free(h);
        free(c);
        TB_ERROR_SET(err, TB_DOMAIN_CORE, TB_CORE_NO_MEMORY, "tb_http_net");
        return NULL;
    }
    mbedtls_x509_crt_init(&c->ca);
    if (net_load_ca(c, ca_file, err) != 0) {
        net_free_ctx(c);
        free(h);
        return NULL;
    }
    c->ua = strdup(user_agent ? user_agent : "tradebot-core/0.3");
    h->name = "net";
    h->ctx = c;
    h->destroy = net_destroy;
    h->request = net_request;
    return h;
}

void tb_http_destroy(tb_http_t *h)
{
    if (h && h->destroy)
        h->destroy(h);
}
