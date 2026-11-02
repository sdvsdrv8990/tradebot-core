/* ==========================================================================
 *  tb/exec/engine.h — механизм торговли (Execution Engine).
 *
 *  Перенос контракта-референса MultiHopSwap (Solidity/Uniswap V3) на
 *  конвейер реестра событий ядра. Одна сделка = одно событие EXEC_SWAP,
 *  прогоняемое цепочкой шагов:
 *
 *    preflight -> ratelimit -> breaker -> quote -> balance -> approve ->
 *    execute -> settle -> commit
 *
 *    preflight  — валидация ордера (ZeroAmount, DeadlineInPast, InvalidPath,
 *                 согласованность маршрута, лимиты риска);
 *    ratelimit  — шлюз token-bucket на бюджет DEX-бэкенда (готовый шаг);
 *    breaker    — шлюз предохранителя бэкенда (готовый шаг);
 *    quote      — ожидаемый выход; amount_out_min == 0 -> авто-минимум
 *                 от политики проскальзывания;
 *    balance    — торговый слот: списание входа, wrap нативного
 *                 (аналог _receiveInput + weth.deposit);
 *    approve    — allowance «роутеру» ровно на сумму входа
 *                 (аналог forceApprove(router, amountIn));
 *    execute    — отправка/поллинг бэкенда: FILLED / SUBMITTED (PENDING,
 *                 блокировка до заполнения) / REJECTED (retryable ->
 *                 backoff и полный повтор цепочки);
 *    settle     — зачисление выхода, unwrap нативного (аналог
 *                 _unwrapAndSend), сброс allowance в 0;
 *    commit     — атомарная фиксация слота (аналог завершения транзакции),
 *                 запись trade.executed в Event Ledger.
 *
 *  Любой отказ на любом шаге = терминальное FAILED: наблюдатель движка
 *  откатывает слот целиком (revert) и пишет trade.failed. Частичных
 *  мутаций балансов не бывает — это главный инвариант, перенесённый из
 *  «всё внутри одной транзакции».
 *
 *  Записи журнала (конверт-схема, source = имя бэкенда, operation =
 *  "swap_exact_in"): trade.submitted (PENDING), trade.executed (SUCCESS),
 *  trade.failed / trade.canceled (ERROR). Терминальные события дополнительно
 *  уходят в журнал как event.trace (мост tb_ev_ledger_attach).
 * ========================================================================== */
#ifndef TB_EXEC_ENGINE_H
#define TB_EXEC_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "tb/core/error.h"
#include "tb/events/events.h"
#include "tb/exec/backend.h"
#include "tb/exec/codes.h"
#include "tb/exec/route.h"
#include "tb/exec/u256.h"
#include "tb/ledger/ledger.h"

#ifdef __cplusplus
extern "C" {
#endif

/* kind события реестра для сделки ('E''X'). */
#define TB_EXEC_KIND_SWAP 0x4558u

#define TB_EXEC_ALLOW_MAX 32 /* хабы всех цепей + токены пулов;
                                 * 16 молча резало при росте сетки */
#define TB_EXEC_MAX_OPEN  64

/* ---- Ордер (exact-input, как в контракте) ---- */
typedef struct tb_exec_order {
    tb_addr_t token_in;       /* NATIVE (нулевой адрес) = нативный актив */
    tb_addr_t token_out;      /* NATIVE = получить нативный */
    tb_u256_t amount_in;      /* точный вход */
    tb_u256_t amount_out_min; /* минимум выхода; 0 -> авто из quote и slippage_bp */
    uint64_t  deadline_ms;    /* epoch ms (аналог block.timestamp) */
    tb_route_t route;         /* маршрут; начало = wrapped(token_in),
                                 конец = wrapped(token_out) */
} tb_exec_order_t;

/* ---- Конфигурация движка (нулевые поля получают значения по умолчанию) ---- */
typedef struct tb_exec_cfg {
    /* риск-политики */
    uint32_t   max_hops;             /* 0 -> TB_EXEC_MAX_HOPS */
    tb_u256_t  max_amount_in;        /* ноль -> без лимита */
    uint32_t   max_slippage_bp;      /* авто-минимум; 0 -> 50 б.п. */
    uint64_t   min_deadline_ahead_ms;/* запас жизни ордера; 0 -> 500 мс */
    tb_addr_t  allow[TB_EXEC_ALLOW_MAX]; /* пусто -> разрешено всё */
    uint32_t   allow_n;

    /* шлюзы бэкенда (как у источников Universal API Runtime) */
    uint32_t   rl_rate_per_min;      /* 0 -> 30 */
    uint32_t   rl_burst;             /* 0 -> 8 */
    uint32_t   br_threshold;         /* 0 -> 3 */
    uint64_t   br_cooldown_ms;       /* 0 -> 3000 */
    uint64_t   backoff_base_ms;      /* 0 -> 700 */
    uint64_t   backoff_cap_ms;       /* 0 -> 5000 */
    double     backoff_jitter;       /* < 0 -> 0.25; 0 = без jitter */

    /* PENDING: поллинг заполнения */
    uint32_t   fill_poll_max;        /* 0 -> 8 */
    uint64_t   fill_poll_interval_ms;/* 0 -> 500 */

    /* бэкенд */
    tb_addr_t  wrapped_native;       /* ОБЯЗАТЕЛЬНО: адрес wrapped-актива */
    tb_exec_backend_t *backend;      /* заимствованный бэкенд (v0.3:
                                        DEX-адаптер вместо симулятора).
                                        Движок НЕ разрушает его (владение у
                                        вызывающего). NULL -> симулятор.
                                        При script != NULL игнорируется
                                        (сценарный симулятор — для тестов). */
    const tb_exec_sim_scenario_t *script; /* сценарии симулятора; NULL ->
                                             всегда немедленное заполнение */
    uint32_t   script_n;
    uint64_t   seed;                 /* сид цен симулятора */
} tb_exec_cfg_t;

/* ---- Статистика ---- */
typedef struct tb_exec_stats {
    uint64_t submitted;
    uint64_t filled;
    uint64_t failed;
    uint64_t canceled;
    uint64_t pending_records;   /* записей trade.submitted */
    uint64_t slippage_rejects;  /* FAILED по TB_EXEC_SLIPPAGE */
    uint64_t backend_rejects;   /* FAILED по отказу бэкенда */
    uint64_t timeouts;          /* FAILED по TB_EXEC_TIMEOUT */
    uint64_t retries;           /* полных повторов цепочки после retryable */
} tb_exec_stats_t;

typedef struct tb_exec_engine tb_exec_engine_t;

tb_exec_engine_t *tb_exec_create(const tb_exec_cfg_t *cfg, tb_error_t *err);
void              tb_exec_destroy(tb_exec_engine_t *e);

/* Реестр движка (для моста к планировщику — см. events_demo — и инспекции). */
tb_evreg_t *tb_exec_registry(tb_exec_engine_t *e);

/* Кошелёк виртуальных балансов (депозиты/инвентаризация). */
struct tb_wallet;
struct tb_wallet *tb_exec_wallet(tb_exec_engine_t *e);

/* Журнал: терминалы дополнительно пишутся как event.trace. */
int tb_exec_attach_ledger(tb_exec_engine_t *e, tb_ledger_t *ledger);

/* Котировка маршрута без ордера (тот же бэкенд, без состояния). */
int tb_exec_quote(tb_exec_engine_t *e, const tb_route_t *route,
                  const tb_u256_t *amount_in, tb_u256_t *out, tb_error_t *err);

/* Подать ордер. Возврат — id события реестра (он же request_id записей
 * конверта), 0 — ошибка подачи (err заполнен). Итог сделки — в состоянии
 * события и записях журнала; PENDING-сделки продолжаются по wake_at_ms
 * (мост к планировщику — как в events_demo). */
uint64_t tb_exec_submit(tb_exec_engine_t *e, const tb_exec_order_t *order,
                        tb_error_t *err);

const tb_exec_stats_t *tb_exec_stats(const tb_exec_engine_t *e);

#ifdef __cplusplus
}
#endif
#endif /* TB_EXEC_ENGINE_H */
