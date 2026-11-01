/* ==========================================================================
 *  tb/core/ids.h — решётка идентификации: группы-владельцы и команды.
 *
 *  Группа = зона ответственности модуля: «кто сообщил/не донёс».
 *  Каждый .c файла ядра объявляет в первой строке
 *
 *      #define TB_CURRENT_GROUP TB_G_...
 *
 *  (до всех include) — макрос TB_ERROR_SET подхватывает её, как TB_LOC.
 *  Ид заморожен явно: значение не меняется никогда (на него ссылаются
 *  журналы), новые группы добавляются только в конец. Имя файла в
 *  комментарии — документация, не источник деривации: файл можно
 *  переименовывать свободно.
 *
 *  Команда (tb_cmd_id_t) — «как развернуть подробности» ошибки: ленивое
 *  расширение контекста по требованию (кнопка «подробнее»). Команда
 *  принадлежит группе; экспандеры регистрируются в реестре.
 * ==========================================================================
 */
#ifndef TB_CORE_IDS_H
#define TB_CORE_IDS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Группы-владельцы (заморожены; добавлять только в конец) ---- */
typedef enum {
    TB_G_UNASSIGNED = 0,  /* не задан (файл без TB_CURRENT_GROUP) */
    TB_G_CORE        = 1, /* core/error.c, log.c, time.c, alloc.c */
    TB_G_REGISTRY    = 2, /* core/error_registry.c — в т.ч. коды TB_API_* */
    TB_G_ARCH        = 3, /* arch/arch.c */
    TB_G_SCHED       = 4, /* sched/loop.c, task.c, pqueue.c, timer.c */
    TB_G_POOL        = 5, /* sched/threadpool.c */
    TB_G_BACKOFF     = 6, /* sched/backoff.c */
    TB_G_RATELIMIT   = 7, /* runtime/ratelimit.c */
    TB_G_BREAKER     = 8, /* runtime/breaker.c */
    TB_G_EVENTS      = 9, /* events/events.c */
    TB_G_BUILTINS    = 10,/* events/builtins.c (шаги-шлюзы, мосты) */
    TB_G_LEDGER      = 11,/* ledger/ledger.c */
    TB_G_JSON        = 12,/* json/envelope.cpp */
    TB_G_CONFIG      = 13,/* json/config.cpp */
    TB_G_NET         = 14,/* net/http.c — транспорт */
    TB_G_EXEC        = 15,/* exec/engine.c — конвейер исполнения */
    TB_G_SIM         = 16,/* exec/backend_sim.c */
    TB_G_DEX         = 17,/* exec/backend_dex.c — живой адаптер */
    TB_G_FILTERS     = 18,/* user/filters.c — фильтры 1–4 */
    TB_G_DECISION    = 19,/* user/decision.c — Decision Engine */
    TB_G_ARB         = 20,/* user/arb.c — рантайм конвейера */
    TB_G_DEMO        = 21 /* демо (вне библиотек) */
} tb_group_id_t;

/* Короткое имя группы ("dex"); для неизвестного — "g%u". */
const char *tb_group_name(uint16_t group);

/* ---- Команды разворачивания деталей (принадлежат группам) ---- */
typedef enum {
    TB_CMD_NET_RAW       = 1, /* g=net: сырой HTTP-ответ (статус/тело) */
    TB_CMD_DEX_HOP       = 2, /* g=dex: вызов хопа — to/calldata/result */
    TB_CMD_DECIDE_CURVE  = 3  /* g=decision: точки profit(size) поиска */
} tb_cmd_id_t;

#ifdef __cplusplus
}
#endif
#endif /* TB_CORE_IDS_H */
