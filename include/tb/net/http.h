/* ==========================================================================
 *  tb/net/http.h — HTTP-транспорт (v0.3, итерация «живые данные»).
 *
 *  Транспорт — vtable с синхронным request(): вызывающие — воркеры пула
 *  потоков, продолжения возвращаются в цикл (tb_http_async, образец —
 *  bot_demo). Две реализации:
 *
 *    tb_http_net_create()    — сокеты + TLS (mbedTLS, вендор) + разбор
 *                              ответа; заголовки/тело в фиксированных
 *                              буферах, усечение помечается;
 *    tb_http_replay_create() — детерминированные ответы по кругу, как
 *                              сценарии симулятора: тесты и офлайн-режим.
 *
 *  Ошибки транспорта — домен TB_DOMAIN_NET (tb_net_code_t). HTTP-статус
 *  ошибкой транспорта НЕ является: он в tb_http_resp_t.status, отображение
 *  статусов на коды API — задача владельца источника
 *  (tb_registry_map_response).
 *
 *  host_pinned: если задан, соединение идёт на указанный IP, а Host и
 *  SNI берутся из url — обход глючного локального DNS-резолвера без
 *  правки системы (аналог curl --resolve).
 * ==========================================================================
 */
#ifndef TB_NET_HTTP_H
#define TB_NET_HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "tb/core/error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Запрос (POD, буферы вызывающего) ---- */
#define TB_HTTP_URL_MAX  512
#define TB_HTTP_HOST_MAX 256

typedef struct tb_http_req {
    char        method[8];          /* "GET"/"POST"; пусто -> GET */
    char        url[TB_HTTP_URL_MAX];  /* https://host[:port]/path[?query] */
    char        host_pinned[TB_HTTP_HOST_MAX]; /* непусто -> IP для соединения */
    const char *body;               /* NULL -> без тела */
    size_t      body_len;
    const char *content_type;       /* NULL -> application/json */
    uint32_t    timeout_ms;         /* 0 -> 10000 */
} tb_http_req_t;

/* ---- Ответ (фиксированные буферы, POD) ---- */
#define TB_HTTP_BODY_MAX   (256 * 1024)
#define TB_HTTP_CTYPE_MAX  64

typedef struct tb_http_resp {
    int32_t  status;                /* HTTP-статус (0 — ответа нет) */
    char     body[TB_HTTP_BODY_MAX];
    size_t   body_len;
    bool     truncated;             /* тело не влезло целиком */
    uint32_t retry_after_ms;        /* из Retry-After (поддержаны секунды) */
    char     content_type[TB_HTTP_CTYPE_MAX];
} tb_http_resp_t;

/* ---- Транспорт ---- */
typedef struct tb_http tb_http_t;
struct tb_http {
    const char *name;               /* "net" / "replay" */
    void       *ctx;
    void (*destroy)(tb_http_t *h);  /* вызывается из tb_http_destroy */
    int  (*request)(tb_http_t *h, const tb_http_req_t *req,
                    tb_http_resp_t *resp, tb_error_t *err);
};

void tb_http_destroy(tb_http_t *h);

/* Синхронный запрос (0 — ответ получен, даже 4xx/5xx; -1 — транспорт
 * не донёс ответ, err заполнен доменом NET). Потокобезопасно. */
static inline int tb_http_request(tb_http_t *h, const tb_http_req_t *req,
                                  tb_http_resp_t *resp, tb_error_t *err)
{
    return h->request(h, req, resp, err);
}

/* ---- Разбор url: scheme/host/port/path ----
 * host: host[:port] без схемы; порт 0 -> 443 (https) / 80 (http);
 * path: "/..." (нет -> "/"). 0 — успех. Экспортировано для тестов. */
int tb_http_parse_url(const char *url, char *host, size_t host_cap,
                      uint16_t *port, char *path, size_t path_cap,
                      bool *is_tls, tb_error_t *err);

/* ---- Реализация: сеть (сокеты + TLS) ----
 * ca_file: NULL -> системные пакеты (Fedora/RHEL /etc/pki/tls/certs/
 * ca-bundle.crt, Debian /etc/ssl/certs/ca-certificates.crt) или
 * переменная окружения TB_CA_FILE. */
tb_http_t *tb_http_net_create(const char *user_agent, const char *ca_file,
                              tb_error_t *err);

/* ---- Реализация: replay (ответы по кругу, как сценарии симулятора) ---- */
typedef struct tb_http_replay_step {
    int32_t     status;
    uint32_t    latency_ms;         /* имитация задержки сети */
    uint32_t    retry_after_ms;
    const char *body;               /* копируется при создании */
} tb_http_replay_step_t;

tb_http_t *tb_http_replay_create(const tb_http_replay_step_t *steps,
                                 size_t n, tb_error_t *err);

#ifdef __cplusplus
}
#endif
#endif /* TB_NET_HTTP_H */
