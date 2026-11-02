/* ==========================================================================
 *  tb/user/webui.h — веб-морда бота: страницы-микромодули + JSON API (v0.5).
 *
 *  Архитектура (единые источники истины + подключаемые страницы):
 *   - данные НЕ живут в морде: приложение кладёт именованные JSON-снапшоты
 *     из потока цикла (tb_webui_set_named), сервер только читает их под
 *     мьютексом; журнал читает собственным read-only fd;
 *   - страница = микромодуль tb_web_page_t {путь, заголовок, JS}: JS сам
 *     фетчит свой /api/<имя> и рисует таблицу. Новая страница = строка в
 *     таблице регистрации + (при желании) новое имя снапшота у приложения;
 *     список поднятых страниц управляется конфигом приложения.
 *
 *  Эндпоинты (формирует сервер):
 *    / и /<path>     — HTML-оболочка + навигация + JS страницы (первая
 *                      зарегистрированная страница открывается на "/");
 *    /api/status     — снапшот с именем "status" (для совместимости);
 *    /api/<имя>      — именованный снапшот (404, если имени нет);
 *    /api/ledger     — хвост JSONL-журнала (все записи);
 *    /api/trades     — только записи trade.* из журнала.
 *
 *  Потоковая модель: сервер в своём потоке, к состоянию бота не обращается.
 *  bind 127.0.0.1 (не торчит наружу); занятый порт — следующий до +20.
 * ==========================================================================
 */
#ifndef TB_USER_WEBUI_H
#define TB_USER_WEBUI_H

#include <stdbool.h>
#include <stdint.h>
#include "tb/core/error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tb_webui tb_webui_t;

#define TB_WEBUI_PAGES_MAX 8
#define TB_WEBUI_SNAPS_MAX 6
#define TB_WEBUI_BIND_MAX 16  /* "255.255.255.255" + NUL */

/* Страница-микромодуль. Все строки — статические (живут всё время работы). */
typedef struct tb_web_page {
    const char *id;    /* ключ конфига: dashboard | venues | trades | … */
    const char *path;  /* URL: "/venues" */
    const char *title; /* пункт навигации */
    const char *js;    /* JS страницы: fetch своего api + рендер в #page */
} tb_web_page_t;

/* Поднять сервер. bind_addr — IPv4 для listen ("127.0.0.1" — только
 * локально, "0.0.0.0" — все интерфейсы, доступ с других машин);
 * NULL -> 127.0.0.1. Фактический порт — в *out (может отличаться,
 * если занят). ledger_path — путь к JSONL-журналу для /api/ledger(trades). */
int tb_webui_start(tb_webui_t **out, uint16_t want_port,
                   const char *bind_addr, const char *ledger_path,
                   tb_error_t *err);

/* Зарегистрировать страницу (потокобезопасно; первая — открывается
 * на "/"). Все строки страницы — статические. 0/-1. */
int tb_webui_page_add(tb_webui_t *w, const tb_web_page_t *page);

/* Обновить именованный JSON-снапшот (потокобезопасно; копируется).
 * "status" — то, что отдаёт /api/status. */
int tb_webui_set_named(tb_webui_t *w, const char *name, const char *json);

/* Совместимость с v0.4: = tb_webui_set_named(w, "status", json). */
void tb_webui_set_json(tb_webui_t *w, const char *json);

uint16_t tb_webui_port(const tb_webui_t *w);

/* Остановить сервер и присоединить поток. Безопасно и без сервера. */
void tb_webui_stop(tb_webui_t *w);

#ifdef __cplusplus
}
#endif
#endif /* TB_USER_WEBUI_H */
