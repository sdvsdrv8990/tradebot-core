/* tests/test_events.c — реестр событий: цепочка модулей, live-таблица,
 * текстовый след шагов, наблюдатели, встроенные шлюзы.
 *
 * Маркеры диагностики включены на уровне этого файла: в выводе теста
 * видно, как выглядит printf-трассировка прогона (библиотека собирается
 * с маркерами отдельной опцией — TB_ENABLE_MARKERS / make markers). */
#define TB_EVENT_MARKERS 1

#include "harness.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "tb/core/marker.h"
#include "tb/events/builtins.h"
#include "tb/events/events.h"

/* ======================================================================
 *  Вспомогательные модули
 * ====================================================================== */

/* Модуль, дописывающий своё имя в общий порядок прохождения. */
typedef struct {
    const char *name;
    char       *order;   /* общий буфер порядка */
} order_mod_t;

static tb_ev_action_t order_fn(tb_evreg_t *r, tb_event_t *ev, void *user)
{
    order_mod_t *m = user;
    tb_event_step(r, ev, "шаг %s", m->name);
    strcat(m->order, m->name);
    return TB_EV_ACT_CHAIN;
}

/* Управляемый модуль: действие задаётся извне. */
typedef struct {
    tb_ev_action_t act;
    uint64_t       wake_at;
    int            calls;
    tb_error_t     err;          /* прикладывается при FAIL */
    bool           attach_err;
} ctl_mod_t;

static tb_ev_action_t ctl_fn(tb_evreg_t *r, tb_event_t *ev, void *user)
{
    ctl_mod_t *m = user;
    m->calls++;
    tb_event_step(r, ev, "управляемый шаг (вызов %d)", m->calls);
    if (m->attach_err)
        tb_event_attach(ev, &m->err);
    if (m->act == TB_EV_ACT_BLOCK)
        tb_event_block_until(ev, m->wake_at);
    return m->act;
}

/* Фейковые часы для детерминированных шлюзов. */
static uint64_t g_now;
static uint64_t fake_now(void) { return g_now; }

/* ======================================================================
 *  1. Регистрация шагов: add / add_before / remove / дубликаты
 * ====================================================================== */
static void test_module_registry(void)
{
    tb_error_t e = TB_ERROR_INIT;
    tb_evreg_t *r = tb_evreg_create(0, &e);
    CHECK(r != NULL);

    char order[64];
    order_mod_t a = { "a", order }, b = { "b", order }, c = { "c", order };

    order[0] = '\0';
    CHECK(tb_evreg_add(r, "a", order_fn, &a, &e) == 0);
    CHECK(tb_evreg_add(r, "b", order_fn, &b, &e) == 1);
    CHECK(tb_evreg_add(r, "c", order_fn, &c, &e) == 2);
    CHECK(tb_evreg_module_count(r) == 3);
    CHECK_STR_EQ(tb_evreg_module_name(r, 1), "b");
    CHECK(tb_evreg_has_module(r, "c"));
    CHECK(!tb_evreg_has_module(r, "zz"));

    /* Дубликат имени отклоняется. */
    tb_error_clear(&e);
    CHECK(tb_evreg_add(r, "a", order_fn, &a, &e) == -1);
    CHECK(e.domain == TB_DOMAIN_EVENTS && e.code == TB_EV_DUPLICATE);

    /* Вставка перед шагом — без переписывания остальных. */
    order_mod_t d = { "d", order };
    CHECK(tb_evreg_add_before(r, "b", "d", order_fn, &d, &e) == 1);
    CHECK_STR_EQ(tb_evreg_module_name(r, 0), "a");
    CHECK_STR_EQ(tb_evreg_module_name(r, 1), "d");
    CHECK_STR_EQ(tb_evreg_module_name(r, 2), "b");
    CHECK_STR_EQ(tb_evreg_module_name(r, 3), "c");

    /* Якорь не найден. */
    tb_error_clear(&e);
    CHECK(tb_evreg_add_before(r, "zz", "x", order_fn, &d, &e) == -1);
    CHECK(e.code == TB_EV_NO_MODULE);

    /* Выгрузка шага. */
    CHECK(tb_evreg_remove(r, "d", &e));
    CHECK(tb_evreg_module_count(r) == 3);
    CHECK(!tb_evreg_has_module(r, "d"));
    tb_error_clear(&e);
    CHECK(!tb_evreg_remove(r, "d", &e));
    CHECK(e.code == TB_EV_NO_MODULE);

    /* Прогон: порядок соответствует цепочке. */
    tb_event_t *ev = tb_event_create(r, 1, 0, NULL);
    CHECK(ev != NULL);
    CHECK(tb_evreg_dispatch(r, ev, &e) == 0);
    CHECK_STR_EQ(order, "abc");
    CHECK(ev->state == TB_EV_DONE);
    CHECK(ev->attempts == 1 && ev->steps == 3);

    tb_evreg_destroy(r);
}

/* ======================================================================
 *  2. Терминальные действия: DONE прерывает, FAIL требует ошибку
 * ====================================================================== */
static void test_terminal_actions(void)
{
    tb_error_t e = TB_ERROR_INIT;
    tb_evreg_t *r = tb_evreg_create(0, NULL);

    /* DONE в середине: хвост цепочки не выполняется. */
    char order[64];
    order_mod_t a = { "a", order }, c = { "c", order };
    ctl_mod_t mid = { .act = TB_EV_ACT_DONE };
    order[0] = '\0';
    tb_evreg_add(r, "a", order_fn, &a, NULL);
    tb_evreg_add(r, "mid", ctl_fn, &mid, NULL);
    tb_evreg_add(r, "c", order_fn, &c, NULL);

    tb_event_t *ev = tb_event_create(r, 2, 0, NULL);
    CHECK(tb_evreg_dispatch(r, ev, &e) == 0);
    CHECK_STR_EQ(order, "a");
    CHECK(ev->state == TB_EV_DONE);
    CHECK(mid.calls == 1);

    /* FAIL с заполненной ошибкой: контекст сохраняется в событии. */
    ctl_mod_t failer = { .act = TB_EV_ACT_FAIL };
    TB_ERROR_SET(&failer.err, TB_DOMAIN_API, TB_API_RATE_LIMIT,
                 "HTTP 429 от источника");
    failer.err.retry_after_ms = 3000;
    failer.err.http_status = 429;
    failer.attach_err = true;

    tb_evreg_remove(r, "mid", NULL);
    tb_evreg_add(r, "fail", ctl_fn, &failer, NULL);

    tb_event_t *ev2 = tb_event_create(r, 3, 0, NULL);
    CHECK(tb_evreg_dispatch(r, ev2, &e) == 0);
    CHECK(ev2->state == TB_EV_FAILED);
    CHECK(ev2->err.domain == TB_DOMAIN_API);
    CHECK(ev2->err.code == TB_API_RATE_LIMIT);
    /* Ошибка — POD по значению: retry_after пережил копирование. */
    CHECK(ev2->err.retry_after_ms == 3000);
    CHECK(ev2->err.http_status == 429);
    CHECK(strstr(ev2->trace, "[fail]") != NULL);

    /* FAIL без ошибки — контрактное нарушение фиксируется реестром. */
    ctl_mod_t sloppy = { .act = TB_EV_ACT_FAIL };
    tb_evreg_remove(r, "fail", NULL);
    tb_evreg_add(r, "sloppy", ctl_fn, &sloppy, NULL);
    tb_event_t *ev3 = tb_event_create(r, 4, 0, NULL);
    CHECK(tb_evreg_dispatch(r, ev3, &e) == 0);
    CHECK(ev3->state == TB_EV_FAILED);
    CHECK(ev3->err.domain == TB_DOMAIN_EVENTS);
    CHECK(ev3->err.code == TB_EV_CONTRACT);
    CHECK(strstr(ev3->err.message, "sloppy") != NULL);

    tb_evreg_destroy(r);
}

/* ======================================================================
 *  3. Блокировка и продолжение (в том числе после выгрузки шага)
 * ====================================================================== */
static void test_block_and_continue(void)
{
    tb_error_t e = TB_ERROR_INIT;
    tb_evreg_t *r = tb_evreg_create(0, NULL);

    char order[64];
    order_mod_t a = { "a", order }, tail = { "z", order };
    ctl_mod_t gate = { .act = TB_EV_ACT_BLOCK, .wake_at = 500 };
    order[0] = '\0';

    tb_evreg_add(r, "a", order_fn, &a, NULL);
    tb_evreg_add(r, "gate", ctl_fn, &gate, NULL);
    tb_evreg_add(r, "z", order_fn, &tail, NULL);

    tb_event_t *ev = tb_event_create(r, 5, 0, NULL);
    CHECK(tb_evreg_dispatch(r, ev, &e) == 0);
    CHECK_STR_EQ(order, "a"); /* 'gate' — управляемый шаг, в порядок не пишет */
    CHECK(ev->state == TB_EV_BLOCKED);
    CHECK_STR_EQ(ev->blocked_by, "gate");
    CHECK(ev->wake_at_ms == 500);

    /* Повторный dispatch из BLOCKED запрещён — только continue. */
    tb_error_clear(&e);
    CHECK(tb_evreg_dispatch(r, ev, &e) == -1);
    CHECK(e.code == TB_EV_BAD_STATE);

    /* Продолжение идёт со шага ПОСЛЕ заблокировавшего. */
    order[0] = '\0';
    CHECK(tb_evreg_continue(r, ev, &e) == 0);
    CHECK_STR_EQ(order, "z");
    CHECK(ev->state == TB_EV_DONE);
    CHECK(ev->attempts == 2);

    /* Выгрузка заблокировавшего шага: продолжение — с начала цепочки. */
    ctl_mod_t gate2 = { .act = TB_EV_ACT_BLOCK, .wake_at = 0 };
    tb_evreg_remove(r, "gate", NULL);
    tb_evreg_add(r, "gate2", ctl_fn, &gate2, NULL);
    tb_event_t *ev2 = tb_event_create(r, 6, 0, NULL);
    CHECK(tb_evreg_dispatch(r, ev2, &e) == 0);
    CHECK(ev2->state == TB_EV_BLOCKED);

    order[0] = '\0';
    tb_evreg_remove(r, "gate2", NULL); /* шаг выгружен, событие ждёт его */
    CHECK(tb_evreg_continue(r, ev2, &e) == 0);
    CHECK_STR_EQ(order, "az"); /* прогон с начала: шлюзы перепроверились */
    CHECK(ev2->state == TB_EV_DONE);
    CHECK(strstr(ev2->trace, "шаг 'gate2' выгружен") != NULL);
    CHECK(tb_evreg_stats(r)->restarts == 1);

    /* continue из QUEUED/DONE — BAD_STATE. */
    tb_event_t *ev3 = tb_event_create(r, 7, 0, NULL);
    tb_error_clear(&e);
    CHECK(tb_evreg_continue(r, ev3, &e) == -1);
    CHECK(e.code == TB_EV_BAD_STATE);
    CHECK(tb_evreg_dispatch(r, ev3, &e) == 0);
    tb_error_clear(&e);
    CHECK(tb_evreg_continue(r, ev3, &e) == -1);
    CHECK(e.code == TB_EV_BAD_STATE);

    tb_evreg_destroy(r);
}

/* ======================================================================
 *  3а. Полный повтор: tb_event_retry_at — прогон с начала цепочки
 * ====================================================================== */
typedef struct {
    int n;
} retry_mod_t;

static tb_ev_action_t retry_fn(tb_evreg_t *r, tb_event_t *ev, void *user)
{
    retry_mod_t *m = user;
    if (m->n++ == 0) {
        tb_event_step(r, ev, "нужен повтор запроса (Retry-After)");
        tb_event_retry_at(ev, 77); /* семантика полного повтора */
        return TB_EV_ACT_BLOCK;
    }
    tb_event_step(r, ev, "повторная попытка");
    return TB_EV_ACT_CHAIN;
}

static void test_retry_flag(void)
{
    tb_error_t e = TB_ERROR_INIT;
    tb_evreg_t *r = tb_evreg_create(0, NULL);

    char order[64];
    order_mod_t a = { "a", order }, z = { "z", order };
    retry_mod_t rmod = { 0 };
    order[0] = '\0';
    tb_evreg_add(r, "a", order_fn, &a, NULL);
    tb_evreg_add(r, "retry", retry_fn, &rmod, NULL);
    tb_evreg_add(r, "z", order_fn, &z, NULL);

    tb_event_t *ev = tb_event_create(r, 1, 0, NULL);
    CHECK(tb_evreg_dispatch(r, ev, &e) == 0);
    CHECK_STR_EQ(order, "a"); /* 'retry' в порядок не пишет */
    CHECK(ev->state == TB_EV_BLOCKED);
    CHECK(ev->wake_at_ms == 77);

    /* Продолжение — с начала цепочки (шлюз 'a' перепроверился). */
    order[0] = '\0';
    CHECK(tb_evreg_continue(r, ev, &e) == 0);
    CHECK_STR_EQ(order, "az"); /* a + retry (без записи) + z */
    CHECK(ev->state == TB_EV_DONE);
    CHECK(ev->attempts == 2);
    CHECK(tb_evreg_stats(r)->restarts == 1);
    CHECK(strstr(ev->trace, "повтор с начала цепочки") != NULL);

    tb_evreg_destroy(r);
}

/* ======================================================================
 *  4. Выключение шага и рекурсия прогона
 * ====================================================================== */
static tb_event_t *g_extra_queued;

static tb_ev_action_t nested_fn(tb_evreg_t *r, tb_event_t *ev, void *user)
{
    tb_error_t e = TB_ERROR_INIT;
    int rc = tb_evreg_dispatch(r, g_extra_queued, &e);
    CHECK(rc == -1);
    CHECK(e.domain == TB_DOMAIN_EVENTS && e.code == TB_EV_RECURSION);
    (void)ev;
    (void)user;
    return TB_EV_ACT_CHAIN;
}

static void test_disable_and_recursion(void)
{
    tb_error_t e = TB_ERROR_INIT;
    tb_evreg_t *r = tb_evreg_create(0, NULL);

    char order[64];
    order_mod_t a = { "a", order }, b = { "b", order };
    order[0] = '\0';
    tb_evreg_add(r, "a", order_fn, &a, NULL);
    tb_evreg_add(r, "b", order_fn, &b, NULL);

    /* Выключенный шаг пропускается, не покидая цепочки. */
    CHECK(tb_evreg_set_enabled(r, "a", false));
    tb_event_t *ev = tb_event_create(r, 8, 0, NULL);
    CHECK(tb_evreg_dispatch(r, ev, &e) == 0);
    CHECK_STR_EQ(order, "b");
    CHECK(ev->steps == 1);

    CHECK(tb_evreg_set_enabled(r, "a", true));
    order[0] = '\0';
    tb_event_t *ev2 = tb_event_create(r, 9, 0, NULL);
    CHECK(tb_evreg_dispatch(r, ev2, &e) == 0);
    CHECK_STR_EQ(order, "ab");

    /* Вложенный прогон запрещён. */
    g_extra_queued = tb_event_create(r, 10, 0, NULL);
    tb_evreg_add(r, "nested", nested_fn, NULL, NULL);
    tb_event_t *ev3 = tb_event_create(r, 11, 0, NULL);
    CHECK(tb_evreg_dispatch(r, ev3, &e) == 0);
    CHECK(g_extra_queued->state == TB_EV_QUEUED); /* не тронуто */

    tb_evreg_destroy(r);
}

/* ======================================================================
 *  5. Live-таблица: find / foreach / close / cancel / вытеснение
 * ====================================================================== */
typedef struct {
    uint32_t mask;
    size_t   count;
} count_ctx_t;

static int visit_count(tb_event_t *ev, void *user)
{
    count_ctx_t *c = user;
    if (c->mask & (1u << ev->state))
        c->count++;
    return 0;
}

static void test_live_table(void)
{
    tb_error_t e = TB_ERROR_INIT;
    tb_evreg_t *r = tb_evreg_create(0, NULL);

    ctl_mod_t blocker = { .act = TB_EV_ACT_BLOCK, .wake_at = 42 };
    tb_evreg_add(r, "gate", ctl_fn, &blocker, NULL);

    tb_event_t *q1 = tb_event_create(r, 1, 0, NULL);
    tb_event_t *q2 = tb_event_create(r, 1, 0, NULL);
    tb_event_t *bl = tb_event_create(r, 2, 0, NULL);
    tb_event_t *d1 = tb_event_create(r, 3, 0, NULL);
    CHECK(q1 && q2 && bl && d1);

    CHECK(tb_evreg_dispatch(r, bl, &e) == 0);
    CHECK(bl->state == TB_EV_BLOCKED);

    /* d1 пропускает шлюз (выключен на время прогона — и это же проверка
     * мягкого выключения в живой таблице). */
    CHECK(tb_evreg_set_enabled(r, "gate", false));
    CHECK(tb_evreg_dispatch(r, d1, &e) == 0);
    CHECK(d1->state == TB_EV_DONE);
    CHECK(tb_evreg_set_enabled(r, "gate", true));

    /* Инспекция: кто в каком состоянии прямо сейчас. */
    CHECK(tb_evreg_count(r, 1u << TB_EV_QUEUED) == 2);
    CHECK(tb_evreg_count(r, 1u << TB_EV_BLOCKED) == 1);
    CHECK(tb_evreg_count(r, 1u << TB_EV_DONE) == 1);
    CHECK(tb_evreg_count(r, TB_EV_LIVE_MASK) == 3);

    count_ctx_t cc = { TB_EV_LIVE_MASK, 0 };
    CHECK(tb_evreg_foreach(r, TB_EV_LIVE_MASK, visit_count, &cc) == 3);
    CHECK(cc.count == 3);

    CHECK(tb_evreg_find(r, q1->id) == q1);
    CHECK(tb_evreg_find(r, 999999) == NULL);

    /* Отмена: QUEUED и BLOCKED можно, терминальные — нет. */
    CHECK(tb_event_cancel(r, q1));
    CHECK(q1->state == TB_EV_CANCELED);
    CHECK(!tb_event_cancel(r, d1));
    CHECK(tb_event_cancel(r, bl));

    /* Закрытие снимает с таблицы. */
    CHECK(tb_event_close(r, q1));
    CHECK(tb_evreg_find(r, q1->id) == NULL);
    CHECK(!tb_event_close(r, q1)); /* уже закрыто */

    /* Вытеснение: терминальное замещается при переполнении. */
    tb_evreg_destroy(r);
    r = tb_evreg_create(4, NULL);
    tb_event_t *e1 = tb_event_create(r, 1, 0, NULL);
    tb_event_t *e2 = tb_event_create(r, 1, 0, NULL);
    tb_event_t *e3 = tb_event_create(r, 1, 0, NULL);
    tb_event_t *e4 = tb_event_create(r, 1, 0, NULL);
    CHECK(e1 && e2 && e3 && e4);
    /* e1, e2 — терминальные (chain пуст -> мгновенный DONE). */
    tb_evreg_dispatch(r, e1, &e);
    tb_evreg_dispatch(r, e2, &e);
    tb_event_t *e5 = tb_event_create(r, 1, 0, NULL);
    CHECK(e5 != NULL); /* вытеснил самое старое терминальное */
    CHECK(tb_evreg_stats(r)->events_recycled == 1);
    tb_event_t *e6 = tb_event_create(r, 1, 0, NULL);
    CHECK(e6 != NULL);
    CHECK(tb_evreg_stats(r)->events_recycled == 2);
    /* Все 4 живые (QUEUED) — нового места нет. */
    tb_error_clear(&e);
    CHECK(tb_event_create(r, 1, 0, &e) == NULL);
    CHECK(e.code == TB_EV_NO_SLOT);

    tb_evreg_destroy(r);
}

/* ======================================================================
 *  6. Наблюдатели состояний (и закрытие из наблюдателя)
 * ====================================================================== */
static int g_obs_calls;
static uint64_t g_obs_last_id;

static void counting_observer(tb_evreg_t *r, tb_event_t *ev, void *user)
{
    (void)r;
    (void)user;
    g_obs_calls++;
    g_obs_last_id = ev->id;
}

static void closing_observer(tb_evreg_t *r, tb_event_t *ev, void *user)
{
    (void)user;
    g_obs_calls++;
    tb_event_close(r, ev); /* допустимо: освобождение отложено до конца рассылки */
}

static void test_observers(void)
{
    tb_error_t e = TB_ERROR_INIT;
    tb_evreg_t *r = tb_evreg_create(0, NULL);

    g_obs_calls = 0;
    CHECK(tb_evreg_on_state(r, 1u << TB_EV_BLOCKED, counting_observer, NULL) >= 0);

    ctl_mod_t gate = { .act = TB_EV_ACT_BLOCK, .wake_at = 7 };
    tb_evreg_add(r, "gate", ctl_fn, &gate, NULL);

    tb_event_t *ev = tb_event_create(r, 1, 0, NULL);
    uint64_t id = ev->id;
    CHECK(tb_evreg_dispatch(r, ev, &e) == 0);
    CHECK(g_obs_calls == 1);
    CHECK(g_obs_last_id == id);

    /* Отписка. */
    CHECK(tb_evreg_off_state(r, counting_observer, NULL));
    CHECK(!tb_evreg_off_state(r, counting_observer, NULL));
    CHECK(tb_evreg_continue(r, ev, &e) == 0);
    CHECK(g_obs_calls == 1); /* больше не оповещается */

    /* Наблюдатель, закрывающий событие: указатели подписчиков валидны. */
    tb_evreg_set_enabled(r, "gate", false); /* ev2 должен дойти до терминала */
    g_obs_calls = 0;
    CHECK(tb_evreg_on_state(r, TB_EV_TERMINAL_MASK, closing_observer, NULL) >= 0);
    tb_event_t *ev2 = tb_event_create(r, 2, 0, NULL);
    uint64_t id2 = ev2->id;
    CHECK(tb_evreg_dispatch(r, ev2, &e) == 0);
    CHECK(g_obs_calls == 1);
    CHECK(tb_evreg_find(r, id2) == NULL); /* закрыт наблюдателем и освобождён */
    CHECK(tb_evreg_stats(r)->events_done == 2); /* ev (через continue) + ev2 */

    tb_evreg_destroy(r);
}

/* ======================================================================
 *  7. Текстовый след: префиксы шагов, note, усечение
 * ====================================================================== */
static tb_ev_action_t chatty_fn(tb_evreg_t *r, tb_event_t *ev, void *user)
{
    (void)user;
    for (int i = 0; i < 12; i++)
        tb_event_step(r, ev, "длинная строка следа номер %d: "
                             "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", i);
    tb_event_note(ev, "заметка без префикса шага");
    return TB_EV_ACT_CHAIN;
}

static void test_trace(void)
{
    tb_error_t e = TB_ERROR_INIT;
    tb_evreg_t *r = tb_evreg_create(0, NULL);
    tb_evreg_add(r, "chatty", chatty_fn, NULL, NULL);

    tb_event_t *ev = tb_event_create(r, 1, 0, NULL);
    CHECK(tb_evreg_dispatch(r, ev, &e) == 0);

    CHECK(strstr(ev->trace, "[chatty] длинная строка следа номер 0") != NULL);
    CHECK(ev->trace[0] == '['); /* след начинается с шага */
    /* Усечение видно явно и посчитано; маркер откатывается по границе
     * символа UTF-8, поэтому длина чуть меньше максимума. */
    size_t len = strlen(ev->trace);
    CHECK(len >= TB_EV_TRACE_MAX - 5 && len <= TB_EV_TRACE_MAX - 1);
    CHECK(strcmp(ev->trace + len - 4, "...\n") == 0);
    CHECK(tb_evreg_stats(r)->notes_truncated >= 1);

    tb_evreg_destroy(r);
}

/* ======================================================================
 *  8. Встроенный шаг rate limit
 * ====================================================================== */
static void test_builtin_ratelimit(void)
{
    tb_error_t e = TB_ERROR_INIT;
    tb_evreg_t *r = tb_evreg_create(0, NULL);

    tb_ratelimit_t rl;
    tb_ratelimit_init(&rl, 12, 2); /* 12/мин: токен каждые 5000 мс */
    tb_evmod_ratelimit_t gate = { .rl = &rl, .now_ms = fake_now };
    tb_evreg_add(r, "ratelimit", tb_evmod_ratelimit_fn, &gate, NULL);

    g_now = 1000;
    tb_event_t *e1 = tb_event_create(r, 1, 0, NULL);
    tb_event_t *e2 = tb_event_create(r, 1, 0, NULL);
    tb_event_t *e3 = tb_event_create(r, 1, 0, NULL);

    CHECK(tb_evreg_dispatch(r, e1, &e) == 0);
    CHECK(e1->state == TB_EV_DONE);
    CHECK(strstr(e1->trace, "granted") != NULL);
    CHECK(tb_evreg_dispatch(r, e2, &e) == 0);
    CHECK(e2->state == TB_EV_DONE);

    /* Третий — отложен до появления токена (12/мин: ~5000 мс + 1 мс запас). */
    CHECK(tb_evreg_dispatch(r, e3, &e) == 0);
    CHECK(e3->state == TB_EV_BLOCKED);
    CHECK_STR_EQ(e3->blocked_by, "ratelimit");
    CHECK(e3->wake_at_ms >= 6000 && e3->wake_at_ms <= 6100);
    CHECK(strstr(e3->trace, "deferred") != NULL);
    CHECK(strstr(e3->trace, "токен через 500") != NULL); /* 5000/5001 */

    /* Часы ушли вперёд — продолжение проходит шлюз. */
    g_now = 6200;
    CHECK(tb_evreg_continue(r, e3, &e) == 0);
    CHECK(e3->state == TB_EV_DONE);

    tb_evreg_destroy(r);
}

/* ======================================================================
 *  9. Встроенный шаг breaker: контекст probe_at (урок t=102)
 * ====================================================================== */
static void test_builtin_breaker(void)
{
    tb_error_t e = TB_ERROR_INIT;
    tb_evreg_t *r = tb_evreg_create(0, NULL);

    tb_breaker_t br;
    tb_breaker_init(&br, 3, 100);
    tb_evmod_breaker_t gate = { .br = &br, .now_ms = fake_now };
    tb_evreg_add(r, "breaker", tb_evmod_breaker_fn, &gate, NULL);

    /* Открыт тремя сбоями в момент t=10: проба с t=110, не со 100. */
    g_now = 10;
    tb_breaker_failure(&br, 10);
    tb_breaker_failure(&br, 10);
    tb_breaker_failure(&br, 10);
    CHECK(br.state == TB_BREAKER_OPEN);
    CHECK(tb_breaker_probe_at_ms(&br) == 110);

    tb_event_t *ev = tb_event_create(r, 1, 0, NULL);
    CHECK(tb_evreg_dispatch(r, ev, &e) == 0);
    CHECK(ev->state == TB_EV_BLOCKED);
    CHECK(ev->wake_at_ms == 110);
    /* Контекст события называет величины явно — арифметики «в голове» нет. */
    CHECK(strstr(ev->trace, "OPEN с t=10") != NULL);
    CHECK(strstr(ev->trace, "cooldown=100") != NULL);
    CHECK(strstr(ev->trace, "probe_at=110") != NULL);

    /* На t=109 — ещё блокировка, на t=110 — проба HALF_OPEN. */
    g_now = 109;
    CHECK(tb_evreg_continue(r, ev, &e) == 0);
    CHECK(ev->state == TB_EV_BLOCKED);
    g_now = 110;
    CHECK(tb_evreg_continue(r, ev, &e) == 0);
    CHECK(ev->state == TB_EV_DONE);
    CHECK(strstr(ev->trace, "проба HALF_OPEN") != NULL);

    /* Проба HALF_OPEN занята первым событием: второе получает короткую
     * перепроверку (не deadlock с wake=0), а после успеха — проходит. */
    tb_event_t *ev2 = tb_event_create(r, 2, 0, NULL);
    CHECK(tb_evreg_dispatch(r, ev2, &e) == 0);
    CHECK(ev2->state == TB_EV_BLOCKED);
    CHECK(ev2->wake_at_ms > 0);
    CHECK(strstr(ev2->trace, "проба занята") != NULL);
    tb_breaker_success(&br);
    g_now = 200;
    CHECK(tb_evreg_continue(r, ev2, &e) == 0);
    CHECK(ev2->state == TB_EV_DONE);

    tb_evreg_destroy(r);
}

/* ======================================================================
 *  10. Мост к Event Ledger: терминальные события уходят в журнал
 * ====================================================================== */
static void test_ledger_bridge(void)
{
    tb_error_t e = TB_ERROR_INIT;
    tb_evreg_t *r = tb_evreg_create(0, NULL);
    tb_ledger_t *led = tb_ledger_open(NULL, 0, &e); /* журнал в памяти */
    CHECK(led != NULL);
    CHECK(tb_ev_ledger_attach(r, led) >= 0);

    /* Успешное событие. */
    tb_event_t *ok = tb_event_create(r, 1, 0, NULL);
    uint64_t ok_id = ok->id;
    CHECK(tb_evreg_dispatch(r, ok, &e) == 0);
    CHECK(ok->state == TB_EV_DONE);
    CHECK(tb_evreg_find(r, ok_id) == NULL); /* закрыт наблюдателем журнала */

    /* Ошибочное событие с сохранённым контекстом. */
    ctl_mod_t failer = { .act = TB_EV_ACT_FAIL };
    TB_ERROR_SET(&failer.err, TB_DOMAIN_API, TB_API_SERVER,
                 "HTTP 503 от источника");
    failer.attach_err = true;
    tb_evreg_add(r, "fail", ctl_fn, &failer, NULL);
    tb_event_t *bad = tb_event_create(r, 2, 0, NULL);
    CHECK(tb_evreg_dispatch(r, bad, &e) == 0);

    CHECK(tb_ledger_last_event_id(led) == 2);
    CHECK(tb_ledger_stats(led)->events_written == 2);
    /* Live-таблица хранит только живые события. */
    CHECK(tb_evreg_count(r, TB_EV_ANY_MASK) == 0);

    tb_evreg_destroy(r);
    tb_ledger_close(led);
}

/* ======================================================================
 *  11. Создание события из шага и статистика модулей
 * ====================================================================== */
static tb_event_t *g_child;

static tb_ev_action_t maker_fn(tb_evreg_t *r, tb_event_t *ev, void *user)
{
    (void)user;
    if (!g_child) {
        g_child = tb_event_create(r, 77, 0, NULL);
        tb_event_step(r, ev, "порождено дочернее событие kind=77");
    }
    return TB_EV_ACT_CHAIN;
}

static void test_origin_and_modstats(void)
{
    tb_error_t e = TB_ERROR_INIT;
    tb_evreg_t *r = tb_evreg_create(0, NULL);

    g_child = NULL;
    CHECK(tb_evreg_add(r, "maker", maker_fn, NULL, NULL) == 0);

    tb_event_t *parent = tb_event_create(r, 1, 0, NULL);
    CHECK(parent->origin == -1); /* создан извне цепочки */
    CHECK(tb_evreg_dispatch(r, parent, &e) == 0);

    CHECK(g_child != NULL);
    CHECK(g_child->origin == 0); /* создан шагом 'maker' (слот 0) */
    CHECK(g_child->state == TB_EV_QUEUED);
    CHECK(g_child->kind == 77);

    tb_evreg_destroy(r);
}

/* ======================================================================
 *  12. Дамп реестра
 * ====================================================================== */
static void test_dump(void)
{
    tb_error_t e = TB_ERROR_INIT;
    tb_evreg_t *r = tb_evreg_create(0, NULL);

    char order[32];
    order_mod_t a = { "a", order };
    ctl_mod_t gate = { .act = TB_EV_ACT_BLOCK, .wake_at = 12345 };
    tb_evreg_add(r, "core", order_fn, &a, NULL);
    tb_evreg_add(r, "gate", ctl_fn, &gate, NULL);

    tb_event_t *q = tb_event_create(r, 1, 0, NULL);
    tb_event_t *b = tb_event_create(r, 2, 0, NULL);
    CHECK(q && b);
    CHECK(tb_evreg_dispatch(r, b, &e) == 0);
    CHECK(b->state == TB_EV_BLOCKED);

    char *buf = NULL;
    size_t size = 0;
    FILE *ms = open_memstream(&buf, &size);
    CHECK(ms != NULL);
    tb_evreg_dump(r, ms);
    fclose(ms);

    CHECK(buf != NULL);
    CHECK(strstr(buf, "цепочка (2 шагов)") != NULL);
    CHECK(strstr(buf, "core -> gate") != NULL);
    CHECK(strstr(buf, "QUEUED 1") != NULL);
    CHECK(strstr(buf, "BLOCKED 1") != NULL);
    CHECK(strstr(buf, "blocked_by=gate") != NULL);
    CHECK(strstr(buf, "[gate]") != NULL);
    CHECK(strstr(buf, "статистика:") != NULL);
    free(buf);

    tb_evreg_destroy(r);
}

/* ====================================================================== */

int main(void)
{
    TB_MARK("test_events: старт");
    test_module_registry();
    test_terminal_actions();
    test_block_and_continue();
    test_retry_flag();
    test_disable_and_recursion();
    test_live_table();
    test_observers();
    test_trace();
    test_builtin_ratelimit();
    test_builtin_breaker();
    test_ledger_bridge();
    test_origin_and_modstats();
    test_dump();
    TB_MARK("test_events: все группы пройдены");
    printf("OK test_events\n");
    return 0;
}
