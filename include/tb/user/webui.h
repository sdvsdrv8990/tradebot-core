/* ==========================================================================
 *  tb/user/webui.h — веб-морда бота: страница + JSON API (v0.4).
 *
 *  Крошечный HTTP-сервер на 127.0.0.1: отдаёт дашборд (одна страница,
 *  автобновление) и два эндпоинта:
 *    /            — HTML-дашборд (статус + последние решения/сделки);
 *    /api/status  — JSON-снапшот состояния (приложение обновляет через
 *                   tb_webui_set_json из потока цикла);
 *    /api/ledger  — хвост JSONL-журнала (последние ~64 КБ, валидные
 *                   строки; чтение собственным fd — гонок с писателем
 *                   нет, частичная последняя строка отбрасывается).
 *
 *  Потоковая модель: сервер живёт в СВОЁМ потоке, к состоянию бота не
 *  обращается — только к снапшоту под мьютексом и к файлу журнала.
 *  bind 127.0.0.1 (не торчит наружу); при занятом порте — следующий
 *  до +20. SO_REUSEADDR — рестарт без TIME_WAIT-ожидания.
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

/* Поднять сервер. Фактический порт — в *out_port (может отличаться,
 * если занят). ledger_path — путь к JSONL-журналу для /api/ledger. */
int tb_webui_start(tb_webui_t **out, uint16_t want_port,
                   const char *ledger_path, tb_error_t *err);

/* Обновить JSON-снапшот статуса (потокобезопасно; копируется). */
void tb_webui_set_json(tb_webui_t *w, const char *json);

uint16_t tb_webui_port(const tb_webui_t *w);

/* Остановить сервер и присоединить поток. Безопасно и без сервера. */
void tb_webui_stop(tb_webui_t *w);

#ifdef __cplusplus
}
#endif
#endif /* TB_USER_WEBUI_H */
