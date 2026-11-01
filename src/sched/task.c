#define TB_CURRENT_GROUP TB_G_SCHED /* зона ответственности файла */
/* ==========================================================================
 *  tb/sched/task.c — жизненный цикл задач и таблица активных задач.
 * ========================================================================== */
#include "tb/sched/task.h"

#include <stdlib.h>
#include <string.h>

const tb_retry_policy_t TB_RETRY_NONE = { 0, 0, 0, 0.0 };

tb_retry_policy_t tb_retry_policy(uint32_t max_attempts,
                                  uint64_t base_ms, uint64_t cap_ms, double jitter)
{
    tb_retry_policy_t p;
    p.max_attempts    = max_attempts;
    p.backoff_base_ms = base_ms;
    p.backoff_cap_ms  = cap_ms;
    p.jitter_frac     = jitter < 0 ? 0 : (jitter > 1 ? 1 : jitter);
    return p;
}

const char *tb_prio_name(tb_prio_t p)
{
    switch (p) {
    case TB_PRIO_CRITICAL:   return "CRITICAL";
    case TB_PRIO_HIGH:       return "HIGH";
    case TB_PRIO_LOW:        return "LOW";
    case TB_PRIO_BACKGROUND: return "BACKGROUND";
    case TB_PRIO_NORMAL:     break;
    }
    return "NORMAL";
}

const char *tb_task_state_name(tb_task_state_t s)
{
    switch (s) {
    case TB_TASK_PENDING:  return "PENDING";
    case TB_TASK_QUEUED:   return "QUEUED";
    case TB_TASK_RUNNING:  return "RUNNING";
    case TB_TASK_DONE:     return "DONE";
    case TB_TASK_FAILED:   return "FAILED";
    case TB_TASK_CANCELED: return "CANCELED";
    }
    return "?";
}

struct tb_task *tb_task_new(uint64_t id, tb_prio_t prio, const char *name,
                            tb_task_fn fn, void *arg, tb_arg_dtor_fn dtor)
{
    struct tb_task *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->id     = id;
    t->prio   = prio;
    t->name   = name ? name : "task";
    t->fn     = fn;
    t->arg    = arg;
    t->dtor   = dtor;
    t->state  = TB_TASK_PENDING;
    t->heap_idx = (size_t)-1;
    return t;
}

void tb_task_free(struct tb_task *t)
{
    if (!t) return;
    if (t->dtor)
        t->dtor(t->arg);
    free(t);
}

/* ---- Компараторы куч ---- */
int tb_task_cmp_ready(const struct tb_task *a, const struct tb_task *b)
{
    if (a->prio != b->prio)
        return a->prio < b->prio ? -1 : 1;
    if (a->seq != b->seq)
        return a->seq < b->seq ? -1 : 1;
    return 0;
}

int tb_task_cmp_deadline(const struct tb_task *a, const struct tb_task *b)
{
    if (a->deadline_ms != b->deadline_ms)
        return a->deadline_ms < b->deadline_ms ? -1 : 1;
    if (a->seq != b->seq)
        return a->seq < b->seq ? -1 : 1;
    return 0;
}

/* ---- Таблица активных задач: открытая адресация, линейное пробирование,
 * удаление со сдвигом назад (без tombstone). Определение — в task.h. ---- */

static uint64_t hash_id(uint64_t id)
{
    /* splitmix64-финализатор — хорошее распределение для плотных id. */
    id += 0x9E3779B97F4A7C15ull;
    id = (id ^ (id >> 30)) * 0xBF58476D1CE4E5B9ull;
    id = (id ^ (id >> 27)) * 0x94D049BB133111EBull;
    return id ^ (id >> 31);
}

bool tb_task_table_init(tb_task_table_t *tbl, unsigned cap_bits)
{
    if (!tbl || cap_bits < 4 || cap_bits > 20) return false;
    tbl->cap = 1ull << cap_bits;
    tbl->mask = tbl->cap - 1;
    tbl->count = 0;
    tbl->slots = calloc(tbl->cap, sizeof(struct tb_task *));
    return tbl->slots != NULL;
}

void tb_task_table_destroy(tb_task_table_t *tbl)
{
    if (!tbl) return;
    for (uint64_t i = 0; i < tbl->cap; i++)
        tb_task_free(tbl->slots[i]);
    free(tbl->slots);
    memset(tbl, 0, sizeof(*tbl));
}

bool tb_task_table_add(tb_task_table_t *tbl, struct tb_task *t)
{
    if (!tbl || !t || tbl->count >= tbl->cap * 3 / 4) return false;
    uint64_t i = hash_id(t->id) & tbl->mask;
    while (tbl->slots[i]) {
        if (tbl->slots[i]->id == t->id) return false; /* уже есть */
        i = (i + 1) & tbl->mask;
    }
    tbl->slots[i] = t;
    tbl->count++;
    return true;
}

struct tb_task *tb_task_table_find(tb_task_table_t *tbl, uint64_t id)
{
    if (!tbl) return NULL;
    uint64_t i = hash_id(id) & tbl->mask;
    while (tbl->slots[i]) {
        if (tbl->slots[i]->id == id) return tbl->slots[i];
        i = (i + 1) & tbl->mask;
    }
    return NULL;
}

void tb_task_table_remove(tb_task_table_t *tbl, struct tb_task *t)
{
    if (!tbl || !t) return;
    uint64_t i = hash_id(t->id) & tbl->mask;
    while (tbl->slots[i]) {
        if (tbl->slots[i] == t) {
            tbl->slots[i] = NULL;
            tbl->count--;
            /* Сдвиг-назад (Кнут, алгоритм R). Элемент из j можно перенести
             * в дыру i, только если его дом k НЕ лежит циклически в (i, j]:
             * иначе поиск от k прошёл бы мимо новой позиции. Условие
             * «k в (i, j]» — это 0 < (k-i) <= (j-i) по модулю ёмкости.
             * (Прежняя проверка (j-i) >= (j-k) была неверна: переносила
             * элементы через их дом и рвала цепочку пробирования — после
             * reap_task в слоте оставался висячий указатель.) */
            uint64_t j = (i + 1) & tbl->mask;
            while (tbl->slots[j]) {
                uint64_t k          = hash_id(tbl->slots[j]->id) & tbl->mask;
                uint64_t hole_span  = (j - i) & tbl->mask;
                uint64_t home_span  = (k - i) & tbl->mask;
                if (home_span == 0 || home_span > hole_span) {
                    tbl->slots[i] = tbl->slots[j];
                    tbl->slots[j] = NULL;
                    i = j;
                }
                j = (j + 1) & tbl->mask;
            }
            return;
        }
        i = (i + 1) & tbl->mask;
    }
}

size_t tb_task_table_count(const tb_task_table_t *tbl)
{
    return tbl ? (size_t)tbl->count : 0;
}
