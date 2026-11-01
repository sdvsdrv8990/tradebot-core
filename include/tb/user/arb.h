/* ==========================================================================
 *  tb/user/arb.h — USER-домен: конвейер арбитража (v0.3).
 *
 *  Фильтры 1–4 и Decision Engine — шаги над снапшотами рынка; ядро
 *  вычислений — ЧИСТЫЕ функции (без сети и без состояния), котировки
 *  приходят через колбэк tb_arb_quote_fn: тесты гоняют синтетические
 *  кривые, живой режим — QuoterV2 через tb_dex.
 *
 *  Математика («тупая и надёжная»):
 *    profit(size) = sell(size) − size − gas_est − min_profit,
 *  где sell(size) — точная цепная котировка WETH -> TOKEN -> WETH через
 *  пулы двух бирж (комиссии и влияние уже в числах Quoter). Ищем
 *  МИНИМАЛЬНЫЙ size из [min_trade, верх контракта], при котором
 *  profit > 0; верх контракта = max_trade (безопасный потолок).
 *  Сделка стала невыгодной до заполнения — отмена: слот откатывается,
 *  теряется только моделируемый газ.
 * ==========================================================================
 */
#ifndef TB_USER_ARB_H
#define TB_USER_ARB_H

#include <stdbool.h>
#include <stdint.h>
#include "tb/core/error.h"
#include "tb/exec/backend_dex.h"
#include "tb/exec/engine.h"
#include "tb/exec/route.h"
#include "tb/exec/u256.h"

#ifdef __cplusplus
extern "C" {
#endif

struct tb_loop;

#define TB_ARB_MAX_POOLS   16
#define TB_ARB_HISTORY_LEN 8

/* ---- Конфигурация ---- */

typedef struct tb_arb_pool {
    tb_addr_t token;  /* второй токен пары (первый — WETH) */
    tb_addr_t pool;   /* адрес пула V3 */
    uint32_t  fee;    /* fee-тир пула */
    char      dex[TB_DEX_NAME_MAX]; /* ожидаемая биржа (safety-фильтр) */
} tb_arb_pool_t;

typedef struct tb_arb_cfg {
    /* venues: RPC + quoter + приписанные пулы (см. tb_exec_dex_cfg_t).
     * network-секция конфига заполняет dex.safe и dex.live. */
    tb_exec_dex_cfg_t dex;
    char              key_file[TB_HTTP_URL_MAX]; /* приватный ключ бота
                                                    (hex), 64 символа */

    tb_addr_t weth; /* ОБЯЗАТЕЛЬНО */

    /* фильтры (0 -> значение по умолчанию) */
    uint32_t freshness_max_ms;     /* 0 -> 3 * poll_ms */
    uint32_t liquidity_probe_mult; /* 0 -> 10 */
    uint32_t liquidity_max_bp;     /* 0 -> 100 (1%) расхождение ставки */
    uint32_t volatility_max_bp;    /* 0 -> 300 (3%) EWMA-разброс мидов */
    char     dex_allow[TB_DEX_MAX_VENUES][TB_DEX_NAME_MAX]; /* топ-N */
    uint32_t dex_allow_n;          /* 0 -> не проверять (все разрешены) */

    /* decision */
    uint64_t poll_ms;         /* 0 -> 8000 */
    uint64_t probe_wei;       /* 0 -> 1e14 */
    uint64_t min_trade_wei;   /* 0 -> 1e14 */
    uint64_t max_trade_wei;   /* 0 -> 1e16 — верх контракта */
    uint64_t min_profit_wei;  /* 0 -> 1e13 */
    uint64_t gas_est_wei;     /* 0 -> 2e14 на цикл */
    uint64_t deadline_ms;     /* 0 -> 8000 окно жизни ордера */
    uint32_t max_pairs_per_poll; /* 0 -> 1 */

    tb_arb_pool_t pools[TB_ARB_MAX_POOLS];
    uint32_t      pools_n;
} tb_arb_cfg_t;

/* Загрузка из JSON (реализация — src/json/config.cpp, rapidjson).
 * Поля, отсутствующие в файле, остаются нулями (дефолты применяет
 * tb_arb_defaults). */
int tb_arb_cfg_load(tb_arb_cfg_t *out, const char *path, tb_error_t *err);

/* Применить дефолты к нулевым полям. */
void tb_arb_defaults(tb_arb_cfg_t *cfg);

/* ---- Снапшот рынка и история ---- */

typedef struct tb_arb_snap {
    uint32_t  pool_idx;  /* индекс в cfg.pools (история по нему же) */
    tb_addr_t pool;
    tb_addr_t token;
    uint32_t  fee;
    char      dex[TB_DEX_NAME_MAX];
    uint64_t  ts_ms;      /* момент котировки */
    tb_u256_t buy_out;    /* TOKEN за probe_wei (WETH -> TOKEN) */
    tb_u256_t big_out;    /* TOKEN за probe_wei * mult (ликвидность) */
    tb_u256_t sell_back;  /* WETH за buy_out (TOKEN -> WETH) */
    bool      complete;   /* все три котировки донесены (контракт
                           * контекста: неполный отсекается фильтрами
                           * с явной причиной, а не «арифметикой») */
} tb_arb_snap_t;

typedef struct tb_arb_hist {
    tb_u256_t buf[TB_ARB_HISTORY_LEN]; /* миды (rate-прокси) */
    uint32_t  n;
    uint32_t  pos;
} tb_arb_hist_t;

/* ---- Вердикты фильтров 1–4 (чистые функции) ---- */

typedef struct tb_arb_verdict {
    bool     pass;           /* все четыре */
    bool     freshness_ok;
    bool     liquidity_ok;
    bool     safety_ok;
    bool     volatility_ok;
    uint64_t age_ms;         /* измеренное */
    uint32_t impact_bp;      /* измеренное расхождение ставок */
    uint32_t vol_bp;         /* измеренный EWMA-разброс */
    char     reject[80];     /* первая причина отказа */
} tb_arb_verdict_t;

/* now_ms — инъекция времени. 0 — прошёл. */
int tb_arb_filter_eval(const tb_arb_cfg_t *cfg, const tb_arb_snap_t *s,
                       const tb_arb_hist_t *h, uint64_t now_ms,
                       tb_arb_verdict_t *v);

/* Добавить мид снапшота в историю (rate = buy_out; фиксируем масштаб
 * probe — конфиг неизменен в рамках прогона). */
void tb_arb_hist_push(tb_arb_hist_t *h, const tb_u256_t *buy_out);

/* ---- Котировщик (инъекция: живой QuoterV2 или синтетика тестов) ----
 * Котировка одного хопа на пуле: tin -> tout, amount -> out. */
typedef int (*tb_arb_quote_fn)(void *ctx, const tb_addr_t *pool,
                               uint32_t fee, const tb_addr_t *tin,
                               const tb_addr_t *tout,
                               const tb_u256_t *amount, tb_u256_t *out,
                               tb_error_t *err);

/* ---- Decision Engine (чистая функция) ---- */

typedef struct tb_arb_pair {
    /* вход */
    const tb_arb_snap_t *buy;  /* пул покупки WETH -> TOKEN */
    const tb_arb_snap_t *sell; /* пул продажи TOKEN -> WETH */
} tb_arb_pair_in_t;

typedef struct tb_arb_pair_res {
    bool     ok;          /* сделка предложена */
    char     reason[80];  /* почему нет (для decision.evaluated) */
    tb_u256_t size;       /* найденная минимальная ставка */
    tb_u256_t buy_out;    /* TOKEN за size (точная) */
    tb_u256_t sell_out;   /* WETH за buy_out (точная) */
    tb_u256_t min_out;    /* size + min_profit + gas */
    uint32_t  probes;     /* итераций поиска размера */
} tb_arb_pair_res_t;

/* Оценить пару: точные цепные котировки через quote_fn, поиск
 * минимального size с profit > 0 геометрическим делением в
 * [min_trade, верх контракта]. Возврат — число записанных результатов
 * (0/1), -1 — ошибка котировщика (err). */
int tb_arb_eval_pair(const tb_arb_cfg_t *cfg, const tb_arb_pair_in_t *p,
                     tb_arb_quote_fn quote, void *qctx,
                     tb_arb_pair_res_t *res, tb_error_t *err);

/* ---- Статистика рантайма ---- */

typedef struct tb_arb_stats {
    uint64_t polls;
    uint64_t quotes;          /* снапшотов собрано */
    uint64_t filter_rejects;  /* снапшотов отсеяно фильтрами */
    uint64_t pairs_evaluated;
    uint64_t pairs_profitable;
    uint64_t quote_errors;     /* пар отброшено по ошибке котировщика */
    uint64_t orders_submitted;
    uint64_t orders_canceled; /* отмены по невыгодности */
    uint64_t cancel_fee_wei;  /* моделируемый потерянный газ */
} tb_arb_stats_t;

/* ---- Рантайм конвейера (реализация src/user/arb.c) ---- */

typedef struct tb_arb tb_arb_t;

/* Создать конвейер: cfg — копируется; engine — заимствован (ордера
 * идут через tb_exec_submit); ledger — заимствован (decision.* записи);
 * http — заимствован транспорт (котировки в воркерах пула). */
tb_arb_t *tb_arb_create(const tb_arb_cfg_t *cfg, tb_exec_engine_t *engine,
                        tb_ledger_t *ledger, tb_http_t *http,
                        tb_error_t *err);
void tb_arb_destroy(tb_arb_t *a);

/* Запуск/остановка опроса (задачи в цикле loop). */
int tb_arb_start(tb_arb_t *a, struct tb_loop *loop, tb_error_t *err);
void tb_arb_stop(tb_arb_t *a);

const tb_arb_stats_t *tb_arb_stats(const tb_arb_t *a);

#ifdef __cplusplus
}
#endif
#endif /* TB_USER_ARB_H */
