/* ==========================================================================
 *  tb/exec/backend.h — бэкенд исполнения: граница с «маршрутизатором».
 *
 *  Роль из контракта-референса:
 *    ISwapRouter (внешний мир) -> tb_exec_backend_t (vtable в нашем ядре).
 *
 *  Сейчас реализован симулятор DEX (backend_sim): детерминированные
 *  котировки из хеша адресов, сценарии заполнения. В итерации «живые
 *  данные» сюда встанет реальный адаптер DEX (HTTP-транспорт, конверты) —
 *  конвейер исполнения не изменится: это та же граница подмены, что и
 *  интерфейс маршрутизатора в контракте.
 *
 *  Контракт вызовов (возвраты):
 *    0  — исполнено сейчас (FILLED): amount_out установлен, сессии нет;
 *    1  — принято в работу (SUBMITTED): tx_id установлен, узнавать результат
 *         поллингом poll(); аналог HTTP 202 / PENDING конверта;
 *   -1  — отказ (REJECTED): err заполнен; retryable-ошибки (серверные)
 *         движок повторит по backoff, остальные — терминальный отказ ордера.
 *
 *  Сличение минимумов — на стороне бэкенда (как amountOutMinimum в
 *  SwapRouter: роутер сам ревертит, если вышло меньше).
 * ========================================================================== */
#ifndef TB_EXEC_BACKEND_H
#define TB_EXEC_BACKEND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "tb/core/error.h"
#include "tb/exec/codes.h"
#include "tb/exec/route.h"
#include "tb/exec/u256.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tb_exec_backend tb_exec_backend_t;

struct tb_exec_backend {
    const char *name;   /* источник в записях конверта ("sim-dex") */
    void       *ctx;    /* состояние реализации */
    void (*destroy)(tb_exec_backend_t *b);

    /* Ожидаемый выход маршрута (без состояния, без мутаций). */
    int (*quote)(tb_exec_backend_t *b, const tb_route_t *route,
                 const tb_u256_t *amount_in, uint64_t now_ms,
                 tb_u256_t *out, tb_error_t *err);

    /* Отправить точный вход в маршрут. */
    int (*submit)(tb_exec_backend_t *b, const tb_route_t *route,
                  const tb_u256_t *amount_in, const tb_u256_t *min_out,
                  uint64_t deadline_ms, uint64_t now_ms,
                  uint64_t *tx_id, tb_u256_t *amount_out, tb_error_t *err);

    /* Узнать судьбу отправленной сессии. */
    int (*poll)(tb_exec_backend_t *b, uint64_t tx_id, uint64_t now_ms,
                tb_u256_t *amount_out, tb_error_t *err);
};

/* ------------------------------------------------------------------ */
/*  Симулятор DEX (детерминированный)                                   */
/* ------------------------------------------------------------------ */

/* Сценарий одного submit (по кругу, как сценарии ответов в bot_demo).
 *   outcome          — чем закончилась отправка;
 *   fill_delay_ms    — SUBMITTED: через сколько заполнится;
 *   out_num/out_den  — выход = котировка * num / den (например 997/1000);
 *   code, retryable  — REJECTED: код ошибки и ретраебельность. */
typedef enum {
    TB_SIM_FILL      = 0, /* исполнено сразу */
    TB_SIM_SUBMIT    = 1, /* принято, заполнится через fill_delay_ms */
    TB_SIM_REJECT    = 2  /* отказ с кодом code */
} tb_sim_outcome_t;

typedef struct tb_exec_sim_scenario {
    uint8_t         outcome;
    uint32_t        fill_delay_ms;
    uint64_t        out_num;
    uint64_t        out_den;
    uint32_t        code;      /* tb_exec_code_t для TB_SIM_REJECT */
    bool            retryable;
    const char     *note;      /* для сообщений/следов */
} tb_exec_sim_scenario_t;

/* Сценарий по умолчанию: немедленное заполнение 99.7% котировки. */
#define TB_SIM_DEFAULT_FILL \
    { TB_SIM_FILL, 0, 997, 1000, TB_EXEC_OK, false, "немедленное заполнение" }

/* Инициализировать backend симулятора (встроенный в tb_exec_backend_t).
 * script == NULL / script_n == 0 — всегда TB_SIM_DEFAULT_FILL. */
int tb_exec_sim_init(tb_exec_backend_t *b, const tb_exec_sim_scenario_t *script,
                     uint32_t script_n, uint64_t seed, tb_error_t *err);

#ifdef __cplusplus
}
#endif
#endif /* TB_EXEC_BACKEND_H */
