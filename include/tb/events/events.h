/* ==========================================================================
 *  tb/events/events.h — модуль регистрации событий (по модулям).
 *
 *  Две цели компонента (зафиксированы в архитектуре):
 *
 *  1. ЦЕПОЧКА ШАГОВ БЕЗ ПЕРЕПИСЫВАНИЯ. Обработка события — это конвейер
 *     зарегистрированных модулей (шагов): core -> ratelimit -> breaker ->
 *     http -> ... Добавить или убрать шаг — это tb_evreg_add() /
 *     tb_evreg_remove() в рантайме; сама цепочка нигде не зашита. Реестр
 *     нужен всем — и ядру, и планировщику, и доменным модулям 1–4/5 —
 *     именно как общий способ собирать обработку события из модулей.
 *
 *  2. СЛЕЖЕНИЕ ЗА ЖИВЫМИ СОБЫТИЯМИ. Реестр ведёт live-таблицу событий
 *     (QUEUED / ACTIVE / BLOCKED + терминальные до закрытия) и даёт ядру,
 *     обработчику и планировщику контекст: что произошло в событии, на
 *     каком этапе оно стоит и что принёс каждый шаг — текстовым следом,
 *     построчно: "[ratelimit] deferred: токен через 4200 мс".
 *
 *  Модель продолжила стиль «коды + контекст» ядра: след шагов и ошибка
 *  события (tb_error_t, чистое POD) накапливаются текстом, запись события
 *  хранит ошибку ПО ЗНАЧЕНИЮ — memcpy-безопасно, копирование без владения.
 *
 *  Два механизма, две роли:
 *   - МОДУЛИ (цепочка) — шаги обработки события; загрузка/выгрузка шага;
 *   - НАБЛЮДАТЕЛИ (подписки) — реакция на переходы состояний: планировщик
 *     (BLOCKED -> таймер на wake_at), Event Ledger (терминал -> JSONL),
 *     метрики. Наблюдатель не может продолжать прогон напрямую (только
 *     планировать его), поэтому рекурсии прогона не возникает.
 *
 *  Контракты:
 *   - реестр однопоточный (поток-владелец, как цикл планировщика);
 *     регистрация из других потоков — постингом в поток-владелец;
 *   - tb_evreg_dispatch() — только из QUEUED; tb_evreg_continue() — только
 *     из BLOCKED (продолжение ПОСЛЕ заблокировавшего шага);
 *   - блокировка двух видов: tb_event_block_until() — «шаг сделал свою
 *     работу и ждёт результата» (полёт запроса), продолжение со
 *     следующего шага; tb_event_retry_at() — «событие должно пройти
 *     цепочку заново» (лимит, предохранитель, повтор запроса после
 *     429/5xx), шлюзы перепроверяются; wake_at_ms == 0 у блокировки —
 *     «ждём внешнего продолжения» без таймера;
 *   - шаги-шлюзы (лимит, предохранитель) обязаны стоять до шагов с
 *     побочными эффектами и блокировать событие через tb_event_retry_at;
 *   - TB_EV_ACT_FAIL обязан заполнить ошибку события (tb_event_attach /
 *     TB_ERROR_SET в ev->err), иначе реестр ставит TB_EV_CONTRACT;
 *   - вложенный dispatch/continue запрещён (TB_EV_RECURSION);
 *   - мутации цепочки из обработчика: add / set_enabled — свободно;
 *     remove — с осторожностью (см. walk_resync в events.c): выгрузка
 *     собственного шага или шага до него в одном вызове не поддерживается.
 * ========================================================================== */
#ifndef TB_EVENTS_EVENTS_H
#define TB_EVENTS_EVENTS_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "tb/core/error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Лимиты (фиксированные буферы — стиль ядра) ---- */
#define TB_EVMOD_NAME_MAX     24   /* имя шага цепочки */
#define TB_EVENTS_MAX_MODULES 32   /* шагов в цепочке */
#define TB_EVENTS_MAX_OBSERVERS 8  /* подписок на состояния */
#define TB_EVENTS_DEFAULT_LIVE 128 /* событий в live-таблице по умолчанию */
#define TB_EV_TRACE_MAX       512  /* текстовый след шагов */

/* ---- Состояния события ---- */
typedef enum {
    TB_EV_QUEUED   = 0,  /* создан, ждёт запуска в цепочку */
    TB_EV_ACTIVE   = 1,  /* идёт по цепочке шагов прямо сейчас */
    TB_EV_BLOCKED  = 2,  /* ждёт: wake_at_ms (таймер) или внешнего продолжения */
    TB_EV_DONE     = 3,  /* терминальный успех */
    TB_EV_FAILED   = 4,  /* терминальная ошибка (см. ev->err) */
    TB_EV_CANCELED = 5   /* отменён владельцем */
} tb_ev_state_t;

#define TB_EV_LIVE_MASK                                                        \
    ((1u << TB_EV_QUEUED) | (1u << TB_EV_ACTIVE) | (1u << TB_EV_BLOCKED))
#define TB_EV_TERMINAL_MASK                                                    \
    ((1u << TB_EV_DONE) | (1u << TB_EV_FAILED) | (1u << TB_EV_CANCELED))
#define TB_EV_ANY_MASK 0xFFu

const char *tb_ev_state_name(tb_ev_state_t s);

/* ---- Действие, возвращаемое шагом цепочки ---- */
typedef enum {
    TB_EV_ACT_CHAIN = 0,  /* передать событие следующему шагу */
    TB_EV_ACT_BLOCK = 1,  /* ждать: см. tb_event_block_until() / контракты выше */
    TB_EV_ACT_DONE  = 2,  /* терминальный успех (цепочка дальше не идёт) */
    TB_EV_ACT_FAIL  = 3   /* терминальная ошибка (ошибка — в событии) */
} tb_ev_action_t;

typedef struct tb_evreg  tb_evreg_t;
typedef struct tb_event  tb_event_t;

/* Шаг цепочки. Возвращает действие для события. */
typedef tb_ev_action_t (*tb_ev_handler_t)(tb_evreg_t *r, tb_event_t *ev,
                                          void *user);

/* ---- Запись события ----
 * Чтение полей разрешено всем; запись — только реестру (кроме flags —
 * пользовательские биты, см. TB_EV_FLAG_USER). */
#define TB_EV_FLAG_USER_MASK 0xF0u  /* пользовательские биты (шаги домена) */
#define TB_EV_F_RESTART      0x01u  /* внутренний флаг: продолжить с начала */

struct tb_event {
    uint64_t   id;            /* монотонный, в пределах реестра не переиспользуется */
    uint16_t   kind;          /* тип события в терминах владельца */
    uint8_t    state;         /* tb_ev_state_t */
    uint8_t    flags;         /* младшие 4 бита — реестр, TB_EV_FLAG_USER_MASK — шаги */
    int32_t    origin;        /* слот модуля-создателя; -1 — создан извне цепочки */
    uint64_t   tag;           /* произвольное значение владельца/шагов (POD, без владения) */
    uint64_t   created_ms;
    uint64_t   updated_ms;
    uint64_t   wake_at_ms;    /* BLOCKED: абсолютный монотонный момент; 0 — вручную */
    uint32_t   attempts;      /* входов в цепочку (dispatch + continue) */
    uint32_t   steps;         /* шагов, через которые событие прошло */
    char       blocked_by[TB_EVMOD_NAME_MAX]; /* кто заблокировал */
    tb_error_t err;           /* контекст ошибки: POD, хранится по значению */
    char       trace[TB_EV_TRACE_MAX];        /* текстовый след шагов */
};

/* ---- Статистика реестра ---- */
typedef struct tb_evreg_stats {
    uint64_t events_created;
    uint64_t events_done;
    uint64_t events_failed;
    uint64_t events_canceled;
    uint64_t events_recycled;   /* терминальные вытеснены при переполнении таблицы */
    uint64_t dispatches;        /* запусков цепочки (с начала) */
    uint64_t continues;         /* продолжений после блокировки */
    uint64_t restarts;          /* продолжений «с начала»: заблокировавший шаг выгружен */
    uint64_t blocked;           /* переходов в BLOCKED */
    uint64_t notes_truncated;   /* записей следа, не поместившихся целиком */
    uint64_t modules_added;
    uint64_t modules_removed;
} tb_evreg_stats_t;

/* ==========================================================================
 *  Реестр: создание и общее
 * ========================================================================== */

/* max_live — ёмкость live-таблицы событий (0 -> TB_EVENTS_DEFAULT_LIVE).
 * При переполнении вытесняется самое старое терминальное событие
 * (его след уже должен быть забран наблюдателем/журналом). */
tb_evreg_t *tb_evreg_create(size_t max_live, tb_error_t *err);
void        tb_evreg_destroy(tb_evreg_t *r);

const tb_evreg_stats_t *tb_evreg_stats(const tb_evreg_t *r);

/* ==========================================================================
 *  Модули — шаги цепочки
 * ========================================================================== */

/* Загрузить шаг в конец цепочки. Имя уникально. Возврат — индекс слота. */
int  tb_evreg_add(tb_evreg_t *r, const char *name, tb_ev_handler_t fn,
                  void *user, tb_error_t *err);

/* Загрузить шаг ПЕРЕД уже зарегистрированным шагом anchor — вставка в
 * середину конвейера без переписывания остальных шагов. */
int  tb_evreg_add_before(tb_evreg_t *r, const char *anchor, const char *name,
                         tb_ev_handler_t fn, void *user, tb_error_t *err);

/* Выгрузить шаг. События, заблокированные этим шагом, при продолжении
 * прогоняются с начала цепочки (см. контракты выше). */
bool tb_evreg_remove(tb_evreg_t *r, const char *name, tb_error_t *err);

/* Мягкое включение/выключение шага без выгрузки из цепочки. */
bool tb_evreg_set_enabled(tb_evreg_t *r, const char *name, bool on);

/* Есть ли шаг с таким именем; позиция шага в цепочке; имя шага по индексу. */
bool        tb_evreg_has_module(const tb_evreg_t *r, const char *name);
int         tb_evreg_mod_index(const tb_evreg_t *r, const char *name);
const char *tb_evreg_module_name(const tb_evreg_t *r, int idx);
size_t      tb_evreg_module_count(const tb_evreg_t *r);

/* ==========================================================================
 *  События: создание и жизненный цикл
 * ========================================================================== */

/* Создать событие (state=QUEUED). origin — текущий шаг, если создание
 * происходит из обработчика, иначе «извне». */
tb_event_t *tb_event_create(tb_evreg_t *r, uint16_t kind, uint8_t flags,
                            tb_error_t *err);

tb_event_t *tb_evreg_find(tb_evreg_t *r, uint64_t event_id);

/* Снять событие с live-таблицы (после терминального состояния или отмены). */
bool tb_event_close(tb_evreg_t *r, tb_event_t *ev);

/* Отмена: QUEUED/BLOCKED -> CANCELED (наблюдатели оповещаются; запись
 * остаётся в таблице до tb_event_close). */
bool tb_event_cancel(tb_evreg_t *r, tb_event_t *ev);

/* ==========================================================================
 *  Прогон цепочки
 * ========================================================================== */

/* Запустить событие с начала цепочки (только из QUEUED). Возврат 0 — прогон
 * состоялся (итог — в ev->state); -1 — ошибка механизма (состояние,
 * рекурсия). Ошибка ШАГА — не ошибка прогона: она хранится в событии
 * (ev->err) и видна наблюдателям/журналу. */
int tb_evreg_dispatch(tb_evreg_t *r, tb_event_t *ev, tb_error_t *err);

/* Продолжить BLOCKED-событие со следующего шага после заблокировавшего.
 * Если тот шаг выгружен — прогон с начала цепочки (шлюзы перепроверятся). */
int tb_evreg_continue(tb_evreg_t *r, tb_event_t *ev, tb_error_t *err);

/* ---- Инструменты шага (вызываются из обработчиков) ---- */

/* Пометить блокировку до абсолютного монотонного момента wake_at_ms.
 * Вызвать ДО возврата TB_EV_BLOCK. Продолжение пойдёт со следующего
 * шага после заблокировавшего (шлюзы уже прошли). */
void tb_event_block_until(tb_event_t *ev, uint64_t wake_at_ms);

/* Блокировка с семантикой ПОЛНОГО ПОВТОРА: на wake_at_ms событие
 * прогоняется с НАЧАЛА цепочки — лимит и предохранитель перепроверятся,
 * шаги с побочными эффектами выполнятся заново. Так моделируется
 * повтор запроса после 429/5xx (Retry-After / backoff). */
void tb_event_retry_at(tb_event_t *ev, uint64_t wake_at_ms);

/* Добавить строку в текстовый след события (без префикса шага). */
void tb_event_note(tb_event_t *ev, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* Добавить строку с префиксом текущего шага: "[ratelimit] ...".
 * Вне прогона префикс — "[?]". */
void tb_event_step(tb_evreg_t *r, tb_event_t *ev, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* Сохранить ошибку в запись события (POD-копирование по значению). */
void tb_event_attach(tb_event_t *ev, const tb_error_t *err);

/* ==========================================================================
 *  Наблюдатели переходов состояний
 * ========================================================================== */

typedef void (*tb_ev_observer_fn)(tb_evreg_t *r, tb_event_t *ev, void *user);

/* Подписаться на переходы (state_mask из TB_EV_*_MASK или TB_EV_ANY_MASK).
 * Возврат — индекс подписки или -1. Наблюдатель может закрывать событие
 * (tb_event_close) — прогон после оповещения его не трогает; продолжение
 * прогона из наблюдателя запрещено (планируйте постингом/таймером). */
int  tb_evreg_on_state(tb_evreg_t *r, uint32_t state_mask,
                       tb_ev_observer_fn fn, void *user);
bool tb_evreg_off_state(tb_evreg_t *r, tb_ev_observer_fn fn, void *user);

/* ==========================================================================
 *  Инспекция: кто что делает прямо сейчас
 * ========================================================================== */

typedef int (*tb_event_visit_fn)(tb_event_t *ev, void *user); /* 0 — идти дальше */

/* Обойти события по маске состояний. Возврат — число посещённых. */
size_t tb_evreg_foreach(tb_evreg_t *r, uint32_t state_mask,
                        tb_event_visit_fn fn, void *user);
size_t tb_evreg_count(const tb_evreg_t *r, uint32_t state_mask);

/* Быстрая диагностика: цепочка шагов + все живые события со следами. */
void tb_evreg_dump(tb_evreg_t *r, FILE *out);

#ifdef __cplusplus
}
#endif
#endif /* TB_EVENTS_EVENTS_H */
