/* ==========================================================================
 *  tb/ledger/ledger.h — единый журнал событий (Event Ledger).
 *
 *  Формат строки — JSON Lines, одна строка = одно событие:
 *    {"event_id":17,"ts_ms":1790000000123,"ts":"2026-11-01T12:00:00.123Z",
 *     "type":"api.response","uuid":"...","payload":{...}}
 *
 *  Свойства:
 *   - append-only: история не переписывается, только дополняется;
 *   - монотонный event_id переживает перезапуск (сканируется при open);
 *   - опциональная дедупликация по uuid: окно на TB_LEDGER_DEDUP_WINDOW
 *     последних хешей (64-бит, crc32c-микс); переполнение окна —
 *     fail-open с предупреждением;
 *   - payload обязан быть валидным JSON без переводов строк (JSONL).
 *
 *  Ограничение v0.1: обработка повторных событий и реорганизаций блоков
 *  (восстановление истории после простоя) — в roadmap; здесь заложен
 *  формат и дедупликация.
 * ========================================================================== */
#ifndef TB_LEDGER_LEDGER_H
#define TB_LEDGER_LEDGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "tb/core/error.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TB_LEDGER_MAX_LINE 4096
#define TB_LEDGER_DEDUP_WINDOW 65536u

enum {
    TB_LEDGER_FLUSH_LINE = 1 << 0, /* fflush после каждой строки */
    TB_LEDGER_FSYNC      = 1 << 1  /* fsync после каждой строки (надёжность) */
};

typedef struct tb_ledger_stats {
    uint64_t events_written;
    uint64_t events_duplicate;
    uint64_t bytes_written;
    uint64_t fsync_count;
} tb_ledger_stats_t;

typedef struct tb_ledger tb_ledger_t;

/* path == NULL -> буфер в памяти (для тестов). */
tb_ledger_t *tb_ledger_open(const char *path, unsigned flags, tb_error_t *err);
void          tb_ledger_close(tb_ledger_t *ledger);

/* Записать событие.
 *   type        — категория ("api.response", "decision", "error.reported"...);
 *   payload_json — валидный JSON-объект (или "null");
 *   uuid        — опциональный ключ дедупликации; дубликат не пишется,
 *                 err заполняется TB_LEDGER_DUP (severity INFO), возврат 0.
 * Возврат: 0 — записано или корректно пропущено; -1 — ошибка записи. */
int tb_ledger_emit(tb_ledger_t *ledger, const char *type,
                   const char *payload_json, const char *uuid,
                   uint64_t *out_event_id, tb_error_t *err);

const tb_ledger_stats_t *tb_ledger_stats(const tb_ledger_t *ledger);

/* Последний присвоенный event_id (переживает перезапуск). */
uint64_t tb_ledger_last_event_id(const tb_ledger_t *ledger);

#ifdef __cplusplus
}
#endif
#endif /* TB_LEDGER_LEDGER_H */
