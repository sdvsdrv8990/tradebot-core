#define TB_CURRENT_GROUP TB_G_LEDGER /* зона ответственности файла */
/* ==========================================================================
 *  tb/ledger/ledger.c — JSONL-журнал событий с дедупликацией.
 * ========================================================================== */
#include "tb/ledger/ledger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tb/arch/arch.h"
#include "tb/core/log.h"
#include "tb/core/time.h"

/* Открытая адресация для хешей uuid (линейное пробирование). */
typedef struct {
    uint64_t *slots;    /* 0 — пусто */
    uint32_t  cap;      /* степень двойки */
    uint32_t  count;
} tb_dedup_t;

struct tb_ledger {
    FILE             *fp;        /* NULL -> память (без записи на диск) */
    char             *path;
    unsigned          flags;
    uint64_t          next_event_id;
    tb_dedup_t        dedup;
    tb_ledger_stats_t stats;
};

/* ---- Дедупликация ---- */
static bool dedup_init(tb_dedup_t *d, uint32_t cap_pow2)
{
    d->cap = cap_pow2;
    d->count = 0;
    d->slots = calloc(cap_pow2, sizeof(uint64_t));
    return d->slots != NULL;
}

static void dedup_destroy(tb_dedup_t *d)
{
    free(d->slots);
    d->slots = NULL;
}

static bool dedup_seen_or_add(tb_dedup_t *d, uint64_t h, bool *overflowed)
{
    *overflowed = false;
    uint32_t mask = d->cap - 1;
    uint32_t i = (uint32_t)(h >> 32) & mask;
    while (d->slots[i]) {
        if (d->slots[i] == h) return true; /* дубликат */
        i = (i + 1) & mask;
    }
    if ((uint64_t)d->count * 4ull >= (uint64_t)d->cap * 3ull) {
        /* Окно заполнено на 3/4: fail-open, новых записей не добавляем. */
        *overflowed = true;
        return false;
    }
    d->slots[i] = h;
    d->count++;
    return false;
}

/* ---- Лёгкая валидация payload: JSON-объект или null, без переводов
 * строк (JSONL). Полная проверка схемы — задача потребителей журнала. ---- */
static bool payload_ok(const char *payload, size_t *len_out)
{
    if (!payload) return false;
    size_t n = strlen(payload);
    if (n == 0 || n > TB_LEDGER_MAX_LINE) return false;
    for (size_t i = 0; i < n; i++)
        if (payload[i] == '\n' || payload[i] == '\r') return false;
    bool ok = payload[0] == '{';
    if (!ok && strcmp(payload, "null") == 0)
        ok = true;
    if (!ok) return false;
    *len_out = n;
    return true;
}

/* Найти event_id последней строки при повторном открытии. */
static uint64_t scan_last_event_id(FILE *fp)
{
    if (!fp) return 0;
    /* Читаем хвост файла (с запасом под терминатор) и ищем последнюю
     * полную строку. */
    char buf[1025];
    if (fseek(fp, 0, SEEK_END) != 0) return 0;
    long size = ftell(fp);
    if (size <= 0) return 0;
    long start = size - 1024;
    if (start < 0) start = 0;
    if (fseek(fp, start, SEEK_SET) != 0) return 0;
    size_t got = fread(buf, 1, 1024, fp);
    if (got == 0) return 0;
    buf[got] = '\0'; /* fread не терминирует — обязаны сами */

    /* Ищем последний '\n', не завершающий буфер. */
    long last_nl = -1;
    for (long i = (long)got - 2; i >= 0; i--)
        if (buf[i] == '\n') { last_nl = i; break; }
    const char *line = (last_nl >= 0) ? buf + last_nl + 1 : buf;
    if (*line == '\0') return 0;

    const char *p = strstr(line, "\"event_id\":");
    if (!p) return 0;
    return strtoull(p + 11, NULL, 10);
}

tb_ledger_t *tb_ledger_open(const char *path, unsigned flags, tb_error_t *err)
{
    tb_ledger_t *led = calloc(1, sizeof(tb_ledger_t));
    if (!led) {
        TB_ERROR_SET(err, TB_DOMAIN_CORE, TB_CORE_NO_MEMORY,
                     "ledger: нет памяти");
        return NULL;
    }
    led->flags = flags;
    led->next_event_id = 1;

    if (path) {
        led->path = strdup(path);
        if (!led->path) {
            free(led);
            TB_ERROR_SET(err, TB_DOMAIN_CORE, TB_CORE_NO_MEMORY,
                         "ledger: нет памяти на путь");
            return NULL;
        }
        led->fp = fopen(path, "a+"); /* append + читаемость для сканирования */
        if (!led->fp) {
            TB_ERROR_SET(err, TB_DOMAIN_LEDGER, TB_LEDGER_IO,
                         "ledger: не открыть '%s'", path);
            free(led->path);
            free(led);
            return NULL;
        }
        uint64_t last = scan_last_event_id(led->fp);
        led->next_event_id = last + 1;
        /* Пишем в конец. */
        fseek(led->fp, 0, SEEK_END);
    }

    if (!dedup_init(&led->dedup, TB_LEDGER_DEDUP_WINDOW)) {
        tb_ledger_close(led);
        TB_ERROR_SET(err, TB_DOMAIN_CORE, TB_CORE_NO_MEMORY,
                     "ledger: нет памяти на окно дедупликации");
        return NULL;
    }
    return led;
}

void tb_ledger_close(tb_ledger_t *ledger)
{
    if (!ledger) return;
    if (ledger->fp) {
        fflush(ledger->fp);
        fclose(ledger->fp);
    }
    free(ledger->path);
    dedup_destroy(&ledger->dedup);
    free(ledger);
}

int tb_ledger_emit(tb_ledger_t *ledger, const char *type,
                   const char *payload_json, const char *uuid,
                   uint64_t *out_event_id, tb_error_t *err)
{
    if (!ledger || !type || !payload_json) {
        TB_ERROR_SET(err, TB_DOMAIN_CORE, TB_CORE_INVALID_ARG,
                     "ledger: emit требует type и payload");
        return -1;
    }
    size_t plen;
    if (!payload_ok(payload_json, &plen)) {
        TB_ERROR_SET(err, TB_DOMAIN_LEDGER, TB_LEDGER_BAD_JSON,
                     "ledger: payload не похож на JSON без переводов строк");
        return -1;
    }

    /* Дедупликация по uuid. */
    if (uuid && uuid[0]) {
        bool overflowed = false;
        uint64_t h = tb_hash64(uuid, strlen(uuid), 0xA5A5A5A5A5A5A5A5ull);
        if (dedup_seen_or_add(&ledger->dedup, h, &overflowed)) {
            ledger->stats.events_duplicate++;
            TB_ERROR_SET(err, TB_DOMAIN_LEDGER, TB_LEDGER_DUP,
                         "ledger: дубликат пропущен (uuid=%s)", uuid);
            if (out_event_id) *out_event_id = 0;
            return 0;
        }
        if (overflowed)
            TB_LOG_WARN("ledger: окно дедупликации заполнено — работаем fail-open");
    }

    char ts[32];
    uint64_t wall = tb_wall_ms();
    tb_format_iso8601(wall, ts, sizeof(ts));
    uint64_t id = ledger->next_event_id++;

    char line[TB_LEDGER_MAX_LINE + 256];
    int n;
    if (uuid && uuid[0])
        n = snprintf(line, sizeof(line),
                     "{\"event_id\":%llu,\"ts_ms\":%llu,\"ts\":\"%s\","
                     "\"type\":\"%s\",\"uuid\":\"%s\",\"payload\":%s}\n",
                     (unsigned long long)id, (unsigned long long)wall, ts,
                     type, uuid, payload_json);
    else
        n = snprintf(line, sizeof(line),
                     "{\"event_id\":%llu,\"ts_ms\":%llu,\"ts\":\"%s\","
                     "\"type\":\"%s\",\"payload\":%s}\n",
                     (unsigned long long)id, (unsigned long long)wall, ts,
                     type, payload_json);
    if (n < 0 || (size_t)n >= sizeof(line)) {
        ledger->next_event_id--; /* id не потрачен */
        TB_ERROR_SET(err, TB_DOMAIN_LEDGER, TB_CORE_OVERFLOW,
                     "ledger: строка события не влезает в буфер");
        return -1;
    }

    if (ledger->fp) {
        size_t written = fwrite(line, 1, (size_t)n, ledger->fp);
        if (written != (size_t)n) {
            TB_ERROR_SET(err, TB_DOMAIN_LEDGER, TB_LEDGER_IO,
                         "ledger: запись в '%s' не удалась",
                         ledger->path ? ledger->path : "?");
            return -1;
        }
        if (ledger->flags & TB_LEDGER_FLUSH_LINE)
            fflush(ledger->fp);
        if (ledger->flags & TB_LEDGER_FSYNC) {
            fflush(ledger->fp);
            fsync(fileno(ledger->fp));
            ledger->stats.fsync_count++;
        }
    }

    ledger->stats.events_written++;
    ledger->stats.bytes_written += (uint64_t)n;
    if (out_event_id) *out_event_id = id;
    tb_error_clear(err);
    return 0;
}

const tb_ledger_stats_t *tb_ledger_stats(const tb_ledger_t *ledger)
{
    return ledger ? &ledger->stats : NULL;
}

uint64_t tb_ledger_last_event_id(const tb_ledger_t *ledger)
{
    return ledger ? ledger->next_event_id - 1 : 0;
}
