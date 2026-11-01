/* ==========================================================================
 *  tb/sched/task.h — задача планировщика.
 *
 *  Задача — единая сущность и для немедленных постингов, и для таймеров,
 *  и для периодических операций. Владелец задачи — цикл (tb_loop_t);
 *  физическое освобождение происходит в потоке цикла.
 *
 *  Приоритеты отражают роли бота: предторговая проверка перед сделкой
 *  важнее регулярного опроса источников, а исторические исследования
 *  не должны мешать живой торговле.
 * ========================================================================== */
#ifndef TB_SCHED_TASK_H
#define TB_SCHED_TASK_H

#include <stdbool.h>
#include <stdint.h>
#include "tb/arch/arch.h"
#include "tb/core/error.h"

#ifdef __cplusplus
extern "C" {
#endif

struct tb_loop;
struct tb_task;
typedef struct tb_task tb_task_t;

/* fn выполняется в потоке цикла; dtor(arg) — при освобождении задачи. */
typedef void (*tb_task_fn)(struct tb_loop *loop, struct tb_task *task, void *arg);
typedef void (*tb_arg_dtor_fn)(void *arg);

typedef enum {
    TB_TASK_PENDING  = 0,   /* ждёт дедлайна в таймерном колесе */
    TB_TASK_QUEUED   = 1,   /* готова, стоит в ready-очереди цикла */
    TB_TASK_RUNNING  = 2,
    TB_TASK_DONE     = 3,
    TB_TASK_FAILED   = 4,   /* помечена tb_task_mark_failed() */
    TB_TASK_CANCELED = 5
} tb_task_state_t;

typedef enum {
    TB_PRIO_CRITICAL   = 0, /* предторговая проверка непосредственно перед сделкой */
    TB_PRIO_HIGH       = 1, /* завершения блокирующих операций, критичные обновления */
    TB_PRIO_NORMAL     = 2, /* регулярный опрос источников данных */
    TB_PRIO_LOW        = 3, /* discovery-сканирование, сбор статистики */
    TB_PRIO_BACKGROUND = 4  /* исторические исследования, архивация */
} tb_prio_t;

const char *tb_prio_name(tb_prio_t p);
const char *tb_task_state_name(tb_task_state_t s);

/* ---- Политика повторов задачи ---- */
typedef struct tb_retry_policy {
    uint32_t max_attempts;    /* включая первую; 0 — без повторов */
    uint64_t backoff_base_ms; /* база экспоненты */
    uint64_t backoff_cap_ms;  /* потолок задержки */
    double   jitter_frac;     /* доля случайного отклонения, 0..1 */
} tb_retry_policy_t;

extern const tb_retry_policy_t TB_RETRY_NONE;
tb_retry_policy_t tb_retry_policy(uint32_t max_attempts,
                                  uint64_t base_ms, uint64_t cap_ms, double jitter);

/* ---- Задача ---- */
struct tb_task {
    uint64_t          id;          /* монотонный ID внутри цикла; 0 — невалиден */
    uint64_t          seq;         /* порядок постановки (честность внутри приоритета) */
    uint64_t          deadline_ms; /* абсолютный монотонный дедлайн */
    uint64_t          period_ms;   /* 0 — разовая; иначе периодическая */
    tb_task_fn        fn;
    tb_arg_dtor_fn    dtor;
    void             *arg;
    const char       *name;        /* диагностика; статическая строка */
    tb_prio_t         prio;
    tb_task_state_t   state;
    bool              failed;      /* ставится tb_task_mark_failed() */
    volatile bool     canceled;    /* потокобезопасная пометка отмены */
    uint32_t          attempt;     /* счётчик попыток (для retry-логики владельца) */
    tb_retry_policy_t retry;

    /* Служебные поля контейнеров — трогать только планировщиком. */
    struct tb_task   *next;        /* список слота таймерного колеса */
    size_t            heap_idx;    /* позиция в куче (ready или overflow) */
    bool              in_wheel;    /* в слоте колеса */
    bool              in_heap;     /* в куче (ready/overflow) */
};

/* Таблица активных задач цикла: id -> задача.
 * Определение вынесено в заголовок, чтобы цикл мог встраивать таблицу
 * по значению (вне публичного API — деталь планировщика). */
typedef struct tb_task_table {
    uint64_t       cap;      /* степень двойки */
    uint64_t       mask;
    uint64_t       count;
    struct tb_task **slots;  /* NULL — пусто */
} tb_task_table_t;

bool         tb_task_table_init(tb_task_table_t *tbl, unsigned cap_bits);
void         tb_task_table_destroy(tb_task_table_t *tbl); /* освобождает задачи */
bool         tb_task_table_add(tb_task_table_t *tbl, struct tb_task *t);
struct tb_task *tb_task_table_find(tb_task_table_t *tbl, uint64_t id);
void         tb_task_table_remove(tb_task_table_t *tbl, struct tb_task *t);
size_t       tb_task_table_count(const tb_task_table_t *tbl);

/* Компараторы куч: ready — приоритет, затем порядок постановки;
 * overflow — дедлайн, затем порядок. */
int tb_task_cmp_ready(const struct tb_task *a, const struct tb_task *b);
int tb_task_cmp_deadline(const struct tb_task *a, const struct tb_task *b);

/* Создание/освобождение — только из потока цикла. */
struct tb_task *tb_task_new(uint64_t id, tb_prio_t prio, const char *name,
                            tb_task_fn fn, void *arg, tb_arg_dtor_fn dtor);
void            tb_task_free(struct tb_task *t);

/* Пометить текущую задачу как неудачную (влияет на state после fn). */
static inline void tb_task_mark_failed(struct tb_task *t) { if (t) t->failed = true; }

#ifdef __cplusplus
}
#endif
#endif /* TB_SCHED_TASK_H */
