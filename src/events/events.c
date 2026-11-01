#define TB_CURRENT_GROUP TB_G_EVENTS /* зона ответственности файла */
/* ==========================================================================
 *  tb/events/events.c — реализация реестра событий.
 *
 *  Ключевые инварианты:
 *   - слот live-таблицы: ev обязано быть первым полем (валидация указателя
 *     событие->слот через адресную арифметику);
 *   - закрытие события из наблюдателя — отложено до конца рассылки
 *     (pending_close): память не переиспользуется под ногами подписчиков;
 *   - обход цепочки пересинхронизируется по имени шага, если обработчик
 *     изменил цепочку (add/remove) в середине прогона;
 *   - имя шага копируется в локальный буфер ДО вызова обработчика:
 *     выгрузивший сам себя шаг затирается memmove сразу по возврату.
 * ========================================================================== */
#include "tb/events/events.h"
#include "tb/core/log.h"
#include "tb/core/marker.h"
#include "tb/core/strutil.h"
#include "tb/core/time.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* ---- Слот шага цепочки ---- */
typedef struct tb_evmod {
    char            name[TB_EVMOD_NAME_MAX];
    tb_ev_handler_t handler;
    void           *user;
    uint32_t        needs;    /* TB_CTX_*: контекст, нужный шагу */
    bool            enabled;
    /* статистика шага */
    uint64_t        seen;     /* событий дошло до шага */
    uint64_t        blocked;  /* заблокировано этим шагом */
    uint64_t        done;     /* завершено успехом этим шагом */
    uint64_t        failed;   /* завершено ошибкой этим шагом */
} tb_evmod_t;

/* ---- Слот live-таблицы (ev — строго первое поле) ---- */
typedef struct {
    tb_event_t ev;
    bool       in_use;
    bool       pending_close; /* закрыт наблюдателем — освободить после рассылки */
} tb_evslot_t;

/* ---- Подписка на переходы состояний ---- */
typedef struct {
    bool              in_use;
    uint32_t          mask;
    tb_ev_observer_fn fn;
    void             *user;
} tb_observer_t;

struct tb_evreg {
    tb_evmod_t   mods[TB_EVENTS_MAX_MODULES];
    size_t       nmods;
    uint32_t     chain_version;   /* растёт на add/remove */

    tb_evslot_t *live;
    size_t       live_cap;
    size_t      *free_stack;      /* индексы свободных слотов (LIFO) */
    size_t       free_top;
    uint64_t     next_id;

    tb_observer_t obs[TB_EVENTS_MAX_OBSERVERS];

    bool         dispatching;
    int          cur;             /* индекс выполняющегося шага (или -1) */
    unsigned     firing;          /* глубина рассылки наблюдателям */

    tb_evreg_stats_t stats;
};

/* ======================================================================
 *  Имена и мелкие хелперы
 * ====================================================================== */

const char *tb_ev_state_name(tb_ev_state_t s)
{
    switch (s) {
    case TB_EV_QUEUED:   return "QUEUED";
    case TB_EV_ACTIVE:   return "ACTIVE";
    case TB_EV_BLOCKED:  return "BLOCKED";
    case TB_EV_DONE:     return "DONE";
    case TB_EV_FAILED:   return "FAILED";
    case TB_EV_CANCELED: return "CANCELED";
    }
    return "?";
}

static bool is_terminal(tb_ev_state_t s)
{
    return s == TB_EV_DONE || s == TB_EV_FAILED || s == TB_EV_CANCELED;
}

/* Слот, которому принадлежит событие (валидация указателя). */
static bool slot_of(const tb_evreg_t *r, const tb_event_t *ev, size_t *idx)
{
    if (!r || !ev) return false;
    uintptr_t base = (uintptr_t)r->live;
    uintptr_t p    = (uintptr_t)ev;
    if (p < base || p >= base + r->live_cap * sizeof(tb_evslot_t))
        return false;
    if ((p - base) % sizeof(tb_evslot_t) != 0)
        return false;
    size_t i = (p - base) / sizeof(tb_evslot_t);
    if (!r->live[i].in_use || &r->live[i].ev != ev)
        return false;
    if (idx) *idx = i;
    return true;
}

static tb_event_t *live_get(tb_evreg_t *r, uint64_t id)
{
    for (size_t i = 0; i < r->live_cap; i++)
        if (r->live[i].in_use && r->live[i].ev.id == id)
            return &r->live[i].ev;
    return NULL;
}

/* ======================================================================
 *  Рассылка наблюдателям (с отложенным освобождением закрытых)
 * ====================================================================== */

static void sweep_pending_closes(tb_evreg_t *r)
{
    for (size_t i = 0; i < r->live_cap; i++) {
        if (r->live[i].pending_close) {
            r->live[i].pending_close = false;
            r->free_stack[r->free_top++] = i;
        }
    }
}

static void fire_observers(tb_evreg_t *r, tb_event_t *ev, tb_ev_state_t st)
{
    TB_MARK("evreg: событие #%llu -> %s",
            (unsigned long long)ev->id, tb_ev_state_name(st));
    r->firing++;
    for (size_t i = 0; i < TB_EVENTS_MAX_OBSERVERS; i++) {
        tb_observer_t *o = &r->obs[i];
        if (!o->in_use || !(o->mask & (1u << st)))
            continue;
        o->fn(r, ev, o->user);
    }
    r->firing--;
    if (r->firing == 0)
        sweep_pending_closes(r);
}

static void set_state(tb_evreg_t *r, tb_event_t *ev, tb_ev_state_t to)
{
    ev->state     = (uint8_t)to;
    ev->updated_ms = tb_now_ms();
    switch (to) {
    case TB_EV_DONE:     r->stats.events_done++;     break;
    case TB_EV_FAILED:   r->stats.events_failed++;   break;
    case TB_EV_CANCELED: r->stats.events_canceled++; break;
    default: break;
    }
    fire_observers(r, ev, to);
}

/* ======================================================================
 *  Создание / уничтожение реестра
 * ====================================================================== */

tb_evreg_t *tb_evreg_create(size_t max_live, tb_error_t *err)
{
    if (max_live == 0)
        max_live = TB_EVENTS_DEFAULT_LIVE;

    tb_evreg_t *r = calloc(1, sizeof(*r));
    if (!r) {
        TB_ERROR_SET(err, TB_DOMAIN_CORE, TB_CORE_NO_MEMORY,
                     "tb_evreg: нет памяти на реестр");
        return NULL;
    }
    r->live       = calloc(max_live, sizeof(*r->live));
    r->free_stack = malloc(max_live * sizeof(*r->free_stack));
    if (!r->live || !r->free_stack) {
        free(r->live);
        free(r->free_stack);
        free(r);
        TB_ERROR_SET(err, TB_DOMAIN_CORE, TB_CORE_NO_MEMORY,
                     "tb_evreg: нет памяти на live-таблицу (%zu)", max_live);
        return NULL;
    }
    r->live_cap = max_live;
    for (size_t i = 0; i < max_live; i++)
        r->free_stack[i] = i;      /* pop выдаёт 0,1,2,... */
    r->free_top = max_live;
    r->next_id  = 1;
    r->cur      = -1;

    TB_MARK("evreg: создан (live=%zu, шагов max %d)",
            max_live, TB_EVENTS_MAX_MODULES);
    return r;
}

void tb_evreg_destroy(tb_evreg_t *r)
{
    if (!r) return;
    TB_MARK("evreg: уничтожен (событий создано %llu)",
            (unsigned long long)r->stats.events_created);
    free(r->live);
    free(r->free_stack);
    free(r);
}

const tb_evreg_stats_t *tb_evreg_stats(const tb_evreg_t *r)
{
    return r ? &r->stats : NULL;
}

/* ======================================================================
 *  Модули — шаги цепочки
 * ====================================================================== */

int tb_evreg_mod_index(const tb_evreg_t *r, const char *name)
{
    if (!r || !name) return -1;
    for (size_t i = 0; i < r->nmods; i++)
        if (strcmp(r->mods[i].name, name) == 0)
            return (int)i;
    return -1;
}

bool tb_evreg_has_module(const tb_evreg_t *r, const char *name)
{
    return tb_evreg_mod_index(r, name) >= 0;
}

const char *tb_evreg_module_name(const tb_evreg_t *r, int idx)
{
    if (!r || idx < 0 || (size_t)idx >= r->nmods) return NULL;
    return r->mods[idx].name;
}

size_t tb_evreg_module_count(const tb_evreg_t *r)
{
    return r ? r->nmods : 0;
}

static int mod_insert_at(tb_evreg_t *r, size_t pos, const char *name,
                         tb_ev_handler_t fn, void *user, tb_error_t *err)
{
    if (tb_evreg_mod_index(r, name) >= 0) {
        TB_ERROR_SET(err, TB_DOMAIN_EVENTS, TB_EV_DUPLICATE,
                     "шаг '%s' уже в цепочке", name);
        return -1;
    }
    if (r->nmods >= TB_EVENTS_MAX_MODULES) {
        TB_ERROR_SET(err, TB_DOMAIN_EVENTS, TB_EV_NO_SLOT,
                     "цепочка полна (%d шагов)", TB_EVENTS_MAX_MODULES);
        return -1;
    }
    if (pos > r->nmods)
        pos = r->nmods;
    memmove(&r->mods[pos + 1], &r->mods[pos],
            (r->nmods - pos) * sizeof(tb_evmod_t));
    tb_evmod_t *m = &r->mods[pos];
    memset(m, 0, sizeof(*m));
    snprintf(m->name, sizeof(m->name), "%s", name);
    m->handler = fn;
    m->user    = user;
    m->enabled = true;
    r->nmods++;
    r->chain_version++;
    r->stats.modules_added++;
    /* Текущий шаг мог сместиться вставкой — держим r->cur актуальным. */
    if (r->dispatching && (int)pos <= r->cur)
        r->cur++;
    TB_MARK("evreg: +шаг '%s' (позиция %zu, всего %zu)", name, pos, r->nmods);
    return (int)pos;
}

int tb_evreg_add(tb_evreg_t *r, const char *name, tb_ev_handler_t fn,
                 void *user, tb_error_t *err)
{
    if (!r || !name || !fn) {
        TB_ERROR_SET(err, TB_DOMAIN_EVENTS, TB_EV_BAD_HANDLE,
                     "tb_evreg_add: пустой реестр/имя/обработчик");
        return -1;
    }
    return mod_insert_at(r, r->nmods, name, fn, user, err);
}

int tb_evreg_add_needs(tb_evreg_t *r, const char *name, tb_ev_handler_t fn,
                       void *user, uint32_t needs, tb_error_t *err)
{
    int at = tb_evreg_add(r, name, fn, user, err);
    if (at < 0)
        return at;
    r->mods[r->nmods - 1].needs = needs;
    return at;
}

uint32_t tb_evreg_step_needs(const tb_evreg_t *r, const char *name)
{
    if (!r || !name)
        return 0;
    int i = tb_evreg_mod_index((tb_evreg_t *)r, name);
    return i >= 0 ? r->mods[i].needs : 0;
}

int tb_evreg_add_before(tb_evreg_t *r, const char *anchor, const char *name,
                        tb_ev_handler_t fn, void *user, tb_error_t *err)
{
    if (!r || !name || !fn || !anchor) {
        TB_ERROR_SET(err, TB_DOMAIN_EVENTS, TB_EV_BAD_HANDLE,
                     "tb_evreg_add_before: пустой реестр/имя/обработчик/якорь");
        return -1;
    }
    int at = tb_evreg_mod_index(r, anchor);
    if (at < 0) {
        TB_ERROR_SET(err, TB_DOMAIN_EVENTS, TB_EV_NO_MODULE,
                     "шаг-якорь '%s' не найден", anchor);
        return -1;
    }
    return mod_insert_at(r, (size_t)at, name, fn, user, err);
}

bool tb_evreg_remove(tb_evreg_t *r, const char *name, tb_error_t *err)
{
    int idx = tb_evreg_mod_index(r, name);
    if (idx < 0) {
        TB_ERROR_SET(err, TB_DOMAIN_EVENTS, TB_EV_NO_MODULE,
                     "шаг '%s' не найден", name ? name : "?");
        return false;
    }
    memmove(&r->mods[idx], &r->mods[idx + 1],
            (r->nmods - (size_t)idx - 1) * sizeof(tb_evmod_t));
    r->nmods--;
    memset(&r->mods[r->nmods], 0, sizeof(tb_evmod_t));
    r->chain_version++;
    r->stats.modules_removed++;
    if (r->dispatching && idx < r->cur)
        r->cur--;
    TB_MARK("evreg: -шаг '%s' (осталось %zu)", name, r->nmods);
    return true;
}

bool tb_evreg_set_enabled(tb_evreg_t *r, const char *name, bool on)
{
    int idx = tb_evreg_mod_index(r, name);
    if (idx < 0) return false;
    r->mods[idx].enabled = on;
    TB_MARK("evreg: шаг '%s' %s", name, on ? "включён" : "выключен");
    return true;
}

/* ======================================================================
 *  События: создание и жизненный цикл
 * ====================================================================== */

static size_t oldest_terminal_slot(const tb_evreg_t *r)
{
    size_t   best = r->live_cap;
    uint64_t best_ms = UINT64_MAX;
    for (size_t i = 0; i < r->live_cap; i++) {
        if (!r->live[i].in_use) continue;
        if (!is_terminal((tb_ev_state_t)r->live[i].ev.state)) continue;
        if (r->live[i].ev.updated_ms <= best_ms) {
            best_ms = r->live[i].ev.updated_ms;
            best = i;
        }
    }
    return best;
}

tb_event_t *tb_event_create(tb_evreg_t *r, uint16_t kind, uint8_t flags,
                            tb_error_t *err)
{
    if (!r) {
        TB_ERROR_SET(err, TB_DOMAIN_EVENTS, TB_EV_BAD_HANDLE,
                     "tb_event_create: реестр пуст");
        return NULL;
    }
    if (r->free_top == 0) {
        size_t victim = oldest_terminal_slot(r);
        if (victim == r->live_cap) {
            TB_ERROR_SET(err, TB_DOMAIN_EVENTS, TB_EV_NO_SLOT,
                         "live-таблица полна живыми событиями (%zu)",
                         r->live_cap);
            return NULL;
        }
        r->stats.events_recycled++;
        r->live[victim].in_use = false;
        r->free_stack[r->free_top++] = victim;
        TB_MARK("evreg: событие #%llu вытеснено из live-таблицы",
                (unsigned long long)r->live[victim].ev.id);
    }
    size_t slot = r->free_stack[--r->free_top];
    tb_event_t *ev = &r->live[slot].ev;
    memset(ev, 0, sizeof(*ev));
    ev->id      = r->next_id++;
    ev->kind    = kind;
    ev->state   = TB_EV_QUEUED;
    ev->flags   = (uint8_t)(flags & TB_EV_FLAG_USER_MASK);
    ev->origin  = (r->dispatching && r->cur >= 0) ? r->cur : -1;
    ev->created_ms = ev->updated_ms = tb_now_ms();
    r->live[slot].in_use        = true;
    r->live[slot].pending_close = false;
    r->stats.events_created++;
    TB_MARK("evreg: +событие #%llu kind=%u", (unsigned long long)ev->id, kind);
    return ev;
}

tb_event_t *tb_evreg_find(tb_evreg_t *r, uint64_t event_id)
{
    return r ? live_get(r, event_id) : NULL;
}

bool tb_event_close(tb_evreg_t *r, tb_event_t *ev)
{
    size_t idx;
    if (!slot_of(r, ev, &idx))
        return false;
    r->live[idx].in_use = false;
    if (r->firing > 0) {
        /* Рассылка идёт: освобождение отложено, чтобы указатели остальных
         * подписчиков оставались валидными. */
        r->live[idx].pending_close = true;
    } else {
        r->free_stack[r->free_top++] = idx;
    }
    TB_MARK("evreg: -событие #%llu", (unsigned long long)ev->id);
    return true;
}

bool tb_event_cancel(tb_evreg_t *r, tb_event_t *ev)
{
    size_t idx;
    if (!slot_of(r, ev, &idx))
        return false;
    if (ev->state != TB_EV_QUEUED && ev->state != TB_EV_BLOCKED)
        return false; /* ACTIVE отменять нельзя (идёт прогон), терминальные — поздно */
    set_state(r, ev, TB_EV_CANCELED);
    return true;
}

/* ======================================================================
 *  Прогон цепочки
 * ====================================================================== */

static int evreg_walk(tb_evreg_t *r, tb_event_t *ev, size_t start, tb_error_t *err)
{
    (void)err;
    r->dispatching = true;

    ev->attempts++;
    ev->blocked_by[0] = '\0';
    ev->state      = TB_EV_ACTIVE; /* без наблюдателей: переход внутрисобытийный */
    ev->updated_ms = tb_now_ms();

    int rc = 0;
    size_t i = start;
    while (i < r->nmods) {
        if (!r->mods[i].enabled) {
            i++;
            continue;
        }
        tb_evmod_t *m = &r->mods[i];
        char modname[TB_EVMOD_NAME_MAX];
        snprintf(modname, sizeof(modname), "%s", m->name); /* до вызова: может выгрузить себя */

        r->cur = (int)i;
        ev->steps++;
        m->seen++;
        uint32_t version = r->chain_version;

        /* Контракт контекста: шагу не донесены нужные ключи — отказ
         * с точным именованием (кто и чего ждёт). */
        if (m->needs && tb_event_ctx_missing(ev, m->needs)) {
            TB_ERROR_SET(&ev->err, TB_DOMAIN_EVENTS, TB_EV_CONTRACT,
                         "шагу '%s' не донесён контекст %08x (есть %08x)",
                         modname, (unsigned)tb_event_ctx_missing(ev, m->needs),
                         (unsigned)ev->ctx_has);
            m->failed++;
            set_state(r, ev, TB_EV_FAILED);
            goto out;
        }

        tb_ev_action_t act = m->handler(r, ev, m->user);

        /* Пересинхронизация: цепочка могла измениться из обработчика. */
        if (r->chain_version != version) {
            int ni = tb_evreg_mod_index(r, modname);
            i = (ni >= 0) ? (size_t)ni + 1 : i;
        } else {
            i++;
        }
        int mi = tb_evreg_mod_index(r, modname);
        tb_evmod_t *mm = mi >= 0 ? &r->mods[mi] : NULL;

        switch (act) {
        case TB_EV_ACT_CHAIN:
            break;

        case TB_EV_ACT_BLOCK:
            snprintf(ev->blocked_by, sizeof(ev->blocked_by), "%s", modname);
            if (mm) mm->blocked++;
            r->stats.blocked++;
            set_state(r, ev, TB_EV_BLOCKED);
            goto out;

        case TB_EV_ACT_DONE:
            if (mm) mm->done++;
            set_state(r, ev, TB_EV_DONE);
            goto out;

        case TB_EV_ACT_FAIL:
            if (!tb_error_is_set(&ev->err)) {
                TB_ERROR_SET(&ev->err, TB_DOMAIN_EVENTS, TB_EV_CONTRACT,
                             "шаг '%s' вернул FAIL без заполненной ошибки события",
                             modname);
                TB_LOG_WARN("evreg: %s", ev->err.message);
            }
            if (mm) mm->failed++;
            set_state(r, ev, TB_EV_FAILED);
            goto out;

        default:
            TB_LOG_WARN("evreg: шаг '%s' вернул неизвестное действие %d — пропускаем",
                        modname, (int)act);
            break;
        }
    }
    /* Конец цепочки без явного терминала — неявный успех. */
    set_state(r, ev, TB_EV_DONE);

out:
    r->cur = -1;
    r->dispatching = false;
    return rc;
}

int tb_evreg_dispatch(tb_evreg_t *r, tb_event_t *ev, tb_error_t *err)
{
    size_t idx;
    if (!slot_of(r, ev, &idx)) {
        TB_ERROR_SET(err, TB_DOMAIN_EVENTS, TB_EV_BAD_HANDLE,
                     "tb_evreg_dispatch: событие не принадлежит реестру");
        return -1;
    }
    if (r->dispatching) {
        TB_ERROR_SET(err, TB_DOMAIN_EVENTS, TB_EV_RECURSION,
                     "вложенный прогон цепочки (событие #%llu)",
                     (unsigned long long)ev->id);
        return -1;
    }
    if (ev->state != TB_EV_QUEUED) {
        TB_ERROR_SET(err, TB_DOMAIN_EVENTS, TB_EV_BAD_STATE,
                     "dispatch возможен только из QUEUED (сейчас %s)",
                     tb_ev_state_name((tb_ev_state_t)ev->state));
        return -1;
    }
    r->stats.dispatches++;
    return evreg_walk(r, ev, 0, err);
}

int tb_evreg_continue(tb_evreg_t *r, tb_event_t *ev, tb_error_t *err)
{
    size_t idx;
    if (!slot_of(r, ev, &idx)) {
        TB_ERROR_SET(err, TB_DOMAIN_EVENTS, TB_EV_BAD_HANDLE,
                     "tb_evreg_continue: событие не принадлежит реестру");
        return -1;
    }
    if (r->dispatching) {
        TB_ERROR_SET(err, TB_DOMAIN_EVENTS, TB_EV_RECURSION,
                     "вложенный прогон цепочки (событие #%llu)",
                     (unsigned long long)ev->id);
        return -1;
    }
    if (ev->state != TB_EV_BLOCKED) {
        TB_ERROR_SET(err, TB_DOMAIN_EVENTS, TB_EV_BAD_STATE,
                     "continue возможен только из BLOCKED (сейчас %s)",
                     tb_ev_state_name((tb_ev_state_t)ev->state));
        return -1;
    }
    r->stats.continues++;
    bool restart = (ev->flags & TB_EV_F_RESTART) != 0;
    ev->flags &= (uint8_t)~TB_EV_F_RESTART;

    size_t start;
    if (restart) {
        /* Полный повтор: шлюзы перепроверятся, побочные шаги выполнятся заново. */
        r->stats.restarts++;
        tb_event_note(ev, "повтор с начала цепочки: шлюзы перепроверяются");
        start = 0;
    } else {
        int bi = tb_evreg_mod_index(r, ev->blocked_by);
        if (bi < 0) {
            /* Заблокировавший шаг выгружен — прогон с начала: шлюзы перепроверятся. */
            r->stats.restarts++;
            tb_event_note(ev, "шаг '%s' выгружен — прогон с начала цепочки",
                          ev->blocked_by);
            start = 0;
        } else {
            start = (size_t)bi + 1;
        }
    }
    return evreg_walk(r, ev, start, err);
}

/* ======================================================================
 *  Инструменты шага
 * ====================================================================== */

void tb_event_block_until(tb_event_t *ev, uint64_t wake_at_ms)
{
    if (!ev) return;
    ev->wake_at_ms = wake_at_ms;
}

void tb_event_retry_at(tb_event_t *ev, uint64_t wake_at_ms)
{
    if (!ev) return;
    ev->wake_at_ms = wake_at_ms;
    ev->flags |= TB_EV_F_RESTART;
}

void tb_event_attach(tb_event_t *ev, const tb_error_t *err)
{
    if (!ev || !err) return;
    ev->err = *err; /* POD: копирование по значению, владения нет */
}

static void trace_append(tb_evreg_t *r, tb_event_t *ev, const char *prefix,
                         const char *fmt, va_list ap)
{
    char chunk[224];
    vsnprintf(chunk, sizeof(chunk), fmt, ap);
    /* vsnprintf мог оборвать многобайтовый символ UTF-8 — откатываемся
     * к последнему полному символу. */
    tb_utf8_fit(chunk, sizeof(chunk));

    size_t len = strlen(ev->trace);
    if (len + 16 >= TB_EV_TRACE_MAX) {
        /* След почти полон (или закрыт маркером усечения) — терминально:
         * иначе маркер «...» мог бы дописаться повторно. */
        if (r) r->stats.notes_truncated++;
        return;
    }
    int n = prefix
        ? snprintf(ev->trace + len, TB_EV_TRACE_MAX - len, "[%s] %s\n", prefix, chunk)
        : snprintf(ev->trace + len, TB_EV_TRACE_MAX - len, "%s\n", chunk);
    if (n < 0)
        return;
    if ((size_t)n >= TB_EV_TRACE_MAX - len) {
        /* Усечение видно явно: хвост следа — "...\n". Если байт на границе
         * — продолжение многобайтового символа, отступаем к его лид-байту,
         * чтобы символ был стёрт целиком (иначе останется сирота-лид). */
        if (r) r->stats.notes_truncated++;
        size_t p = TB_EV_TRACE_MAX - 5;
        if (((unsigned char)ev->trace[p] & 0xC0) == 0x80) {
            size_t j = p;
            while (j > 0 && ((unsigned char)ev->trace[j] & 0xC0) == 0x80)
                j--;
            p = (((unsigned char)ev->trace[j] & 0xC0) == 0xC0) ? j : j + 1;
        }
        ev->trace[p]     = '.';
        ev->trace[p + 1] = '.';
        ev->trace[p + 2] = '.';
        ev->trace[p + 3] = '\n';
        ev->trace[p + 4] = '\0';
    }
}

void tb_event_note(tb_event_t *ev, const char *fmt, ...)
{
    if (!ev || !fmt) return;
    va_list ap;
    va_start(ap, fmt);
    /* Общий счётчик усечений недоступен без реестра — след просто обрезается. */
    trace_append(NULL, ev, NULL, fmt, ap);
    va_end(ap);
}

void tb_event_step(tb_evreg_t *r, tb_event_t *ev, const char *fmt, ...)
{
    if (!ev || !fmt) return;
    const char *prefix = "?";
    if (r && r->cur >= 0 && (size_t)r->cur < r->nmods)
        prefix = r->mods[r->cur].name;
    va_list ap;
    va_start(ap, fmt);
    trace_append(r, ev, prefix, fmt, ap);
    va_end(ap);
}

/* ======================================================================
 *  Наблюдатели
 * ====================================================================== */

int tb_evreg_on_state(tb_evreg_t *r, uint32_t state_mask,
                      tb_ev_observer_fn fn, void *user)
{
    if (!r || !fn) return -1;
    for (size_t i = 0; i < TB_EVENTS_MAX_OBSERVERS; i++) {
        if (r->obs[i].in_use) continue;
        r->obs[i].in_use = true;
        r->obs[i].mask   = state_mask;
        r->obs[i].fn     = fn;
        r->obs[i].user   = user;
        TB_MARK("evreg: +наблюдатель состояний (mask=0x%x)", state_mask);
        return (int)i;
    }
    return -1;
}

bool tb_evreg_off_state(tb_evreg_t *r, tb_ev_observer_fn fn, void *user)
{
    if (!r || !fn) return false;
    for (size_t i = 0; i < TB_EVENTS_MAX_OBSERVERS; i++) {
        if (r->obs[i].in_use && r->obs[i].fn == fn && r->obs[i].user == user) {
            r->obs[i].in_use = false;
            TB_MARK("evreg: -наблюдатель состояний");
            return true;
        }
    }
    return false;
}

/* ======================================================================
 *  Инспекция
 * ====================================================================== */

size_t tb_evreg_foreach(tb_evreg_t *r, uint32_t state_mask,
                        tb_event_visit_fn fn, void *user)
{
    if (!r || !fn) return 0;
    size_t n = 0;
    for (size_t i = 0; i < r->live_cap; i++) {
        if (!r->live[i].in_use) continue;
        tb_event_t *ev = &r->live[i].ev;
        if (!(state_mask & (1u << ev->state))) continue;
        n++;
        if (fn(ev, user) != 0)
            break;
    }
    return n;
}

size_t tb_evreg_count(const tb_evreg_t *r, uint32_t state_mask)
{
    if (!r) return 0;
    size_t n = 0;
    for (size_t i = 0; i < r->live_cap; i++) {
        if (!r->live[i].in_use) continue;
        if (state_mask & (1u << r->live[i].ev.state)) n++;
    }
    return n;
}

void tb_evreg_dump(tb_evreg_t *r, FILE *out)
{
    if (!r) return;
    if (!out) out = stderr;

    fprintf(out, "── реестр событий ────────────────────────────────────────\n");
    fprintf(out, "цепочка (%zu шагов):", r->nmods);
    for (size_t i = 0; i < r->nmods; i++)
        fprintf(out, "%s%s%s%s", i ? " -> " : " ", r->mods[i].name,
                r->mods[i].enabled ? "" : "[выкл]",
                i + 1 < r->nmods ? "" : "");
    fprintf(out, "\n");
    for (size_t i = 0; i < r->nmods; i++) {
        const tb_evmod_t *m = &r->mods[i];
        fprintf(out, "  [%zu] %-12s seen=%llu blocked=%llu done=%llu failed=%llu%s\n",
                i, m->name,
                (unsigned long long)m->seen, (unsigned long long)m->blocked,
                (unsigned long long)m->done, (unsigned long long)m->failed,
                m->enabled ? "" : "  (выключен)");
    }

    size_t nq = tb_evreg_count(r, 1u << TB_EV_QUEUED);
    size_t na = tb_evreg_count(r, 1u << TB_EV_ACTIVE);
    size_t nb = tb_evreg_count(r, 1u << TB_EV_BLOCKED);
    size_t nt = tb_evreg_count(r, TB_EV_TERMINAL_MASK);
    fprintf(out, "live-таблица: QUEUED %zu, ACTIVE %zu, BLOCKED %zu, терминальных %zu\n",
            nq, na, nb, nt);

    for (size_t i = 0; i < r->live_cap; i++) {
        if (!r->live[i].in_use) continue;
        const tb_event_t *ev = &r->live[i].ev;
        fprintf(out, "  #%llu kind=%u %-8s попыток=%u шагов=%u",
                (unsigned long long)ev->id, ev->kind,
                tb_ev_state_name((tb_ev_state_t)ev->state),
                ev->attempts, ev->steps);
        if (ev->state == TB_EV_BLOCKED) {
            if (ev->wake_at_ms > 0) {
                uint64_t now  = tb_now_ms();
                uint64_t left = ev->wake_at_ms > now ? ev->wake_at_ms - now : 0;
                fprintf(out, " wake=+%llums", (unsigned long long)left);
            } else {
                fprintf(out, " wake=вручную");
            }
            fprintf(out, " blocked_by=%s", ev->blocked_by);
        }
        fprintf(out, "\n");
        if (tb_error_is_set(&ev->err)) {
            char line[320];
            tb_error_format(&ev->err, line, sizeof(line));
            fprintf(out, "      ошибка: %s\n", line);
        }
        if (ev->trace[0]) {
            fprintf(out, "      след:\n");
            const char *p = ev->trace;
            while (*p) {
                const char *nl = strchr(p, '\n');
                size_t len = nl ? (size_t)(nl - p) : strlen(p);
                fprintf(out, "        %.*s\n", (int)len, p);
                if (!nl) break;
                p = nl + 1;
            }
        }
    }
    fprintf(out, "статистика: создано=%llu DONE=%llu FAILED=%llu CANCELED=%llu "
                 "BLOCK-переходов=%llu продолжений=%llu перезапусков=%llu вытеснено=%llu\n",
            (unsigned long long)r->stats.events_created,
            (unsigned long long)r->stats.events_done,
            (unsigned long long)r->stats.events_failed,
            (unsigned long long)r->stats.events_canceled,
            (unsigned long long)r->stats.blocked,
            (unsigned long long)r->stats.continues,
            (unsigned long long)r->stats.restarts,
            (unsigned long long)r->stats.events_recycled);
    fprintf(out, "──────────────────────────────────────────────────────────\n");
}
