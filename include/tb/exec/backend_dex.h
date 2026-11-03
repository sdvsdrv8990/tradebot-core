/* ==========================================================================
 *  tb/exec/backend_dex.h — живой DEX-адаптер за tb_exec_backend_t (v0.3).
 *
 *  Итерация «живые данные»: котировки настоящие — на каждый хоп eth_call
 *  к QuoterV2 выбранной биржи (Uniswap V3-совместимый) через публичный
 *  JSON-RPC (без ключа). Комиссии пула и ценовое влияние уже внутри
 *  котировки Quoter — «сумма покупки минус комиссия» честно.
 *
 *  Исполнение — shadow: деньги виртуальные (кошелёк движка и так
 *  виртуальный), заполнение по цене котировки на момент submit с
 *  модельной задержкой в диапазоне [min..max] мс. Подпись транзакций
 *  (secp256k1, газ, nonce) — отдельная итерация.
 *
 *  Пул каждого хопа берётся из tb_route_pools(route, hop); пул приписан
 *  бирже в конфигурации (venue) — адрес пула идентифицирует и биржу, и
 *  quoter, и RPC. Пул не задан / не приписан — ошибка BACKEND.
 *
 *  Контракт движка: single-thread (поток-владелец движка); транспорт
 *  tb_http_t сам потокобезопасен, адаптеру блокировки не нужны.
 * ==========================================================================
 */
#ifndef TB_EXEC_BACKEND_DEX_H
#define TB_EXEC_BACKEND_DEX_H

#include <stdbool.h>
#include <stdint.h>
#include "tb/core/error.h"
#include "tb/exec/backend.h"
#include "tb/exec/route.h"
#include "tb/net/http.h"
#include "tb/exec/safe_exec.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TB_DEX_MAX_VENUES 8
#define TB_DEX_POOLS_PER_VENUE 24
#define TB_DEX_NAME_MAX 24
#define TB_DEX_MAX_SESSIONS 32

/* Тип биржи: метод котировки одного хопа. */
typedef enum tb_dex_kind {
    TB_DEX_V3 = 0,  /* QuoterV2.quoteExactInputSingle: quoter + factory.getPool */
    TB_DEX_V2 = 1,  /* Router.getAmountsOut: router + factory.getPair;
                     * комиссия 0.30% уже внутри котировки */
} tb_dex_kind_t;

/* ---- Биржа (venue): RPC + метод котировки + её пулы ---- */
typedef struct tb_exec_dex_venue {
    char       name[TB_DEX_NAME_MAX];          /* "uniswap-v3" — в записях;
                                                  * уникально (реестр В9) */
    char       rpc_url[TB_HTTP_URL_MAX];       /* JSON-RPC endpoint (HTTPS) */
    char       rpc_host_pinned[TB_HTTP_HOST_MAX]; /* непусто: IP для
                                                     соединения, Host/SNI
                                                     из rpc_url */
    uint8_t    kind;                           /* tb_dex_kind_t; 0 = V3
                                                 * (дефолт прежних конфигов) */
    uint64_t   chain_id;                       /* цепь venue; 0 при загрузке
                                                 * -> network.chain_id */
    tb_addr_t  weth;                           /* хаб-токен цепи; 0 -> общий
                                                 * cfg.weth (заполняет
                                                 * загрузчик) */
    tb_addr_t  quoter;                         /* V3: адрес QuoterV2 биржи */
    tb_addr_t  router;                         /* V2: Router02.getAmountsOut */
    tb_addr_t  factory;                        /* V3: getPool(tokenA,tokenB,
                                                 * fee); V2: getPair(a,b).
                                                 * Ноль — не проверяем */
    uint32_t   timeout_ms;                     /* 0 -> 8000 */

    tb_addr_t  pools[TB_DEX_POOLS_PER_VENUE];  /* пулы биржи: хоп с таким
                                                  пулом исполняется здесь */
    uint32_t   pools_n;
} tb_exec_dex_venue_t;

/* propose-only (P0): колбэк готового предложения SafeTx — вызывающий
 * пишет журнал/файл/консоль. Вызывается из потока submit. */
typedef void (*tb_exec_dex_proposal_cb)(const tb_safe_proposal_t *p,
                                        void *user);

/* settlement v2 (H1.5): атрибуция исполнения конкретному предложению
 * по событиям Safe (ExecutionSuccess/Failure, topic1 = safeTxHash).
 * Вызывается из потока poll движка при росте nonce. */
typedef enum tb_exec_dex_attribution {
    TB_ATTRIB_ERROR = 0,      /* RPC не ответил: проверить не удалось —
                              * НЕ «не найдено» и НЕ «отказ» */
    TB_ATTRIB_FOUND = 1,     /* событие нашего safeTxHash есть */
    TB_ATTRIB_ABSENT = 2,    /* запрос успешен, события нашего хеша нет
                              * (исполнилось другое предложение Safe) */
} tb_exec_dex_attribution_t;

typedef struct tb_exec_dex_execution {
    tb_exec_dex_attribution_t status;
    bool      success;        /* при FOUND: ExecutionSuccess/Failure */
    uint64_t  block;
    uint8_t   safe_tx_hash[32];
    tb_u256_t balance_delta;  /* фактическая дельта (сумма расчёта) */
} tb_exec_dex_execution_t;
typedef void (*tb_exec_dex_execution_cb)(const tb_exec_dex_execution_t *e,
                                         void *user);

typedef struct tb_exec_dex_cfg {
    tb_exec_dex_venue_t venues[TB_DEX_MAX_VENUES];
    uint32_t            venues_n;
    /* shadow-заполнение: задержка в [min..max] мс (джиттер от seed) */
    uint32_t            fill_delay_min_ms;  /* 0 -> 300 */
    uint32_t            fill_delay_max_ms;  /* 0 -> 900 */
    uint64_t            seed;               /* 0 -> tb_rdtsc() */

    /* live: исполнение из Safe — модель ПОДПИСАНИЯ ЧЕЛОВЕКОМ (P0).
     * Котировки те же (QuoterV2); submit ГОТОВИТ SafeTx (propose-only:
     * calldata + EIP-712-хеш + подпись бота) и НЕ отправляет ничего;
     * on_propose (если задан) вызывается из потока submit с готовым
     * предложением. Poll следит за nonce Safe: вырос — исполнено
     * (результат = дельта balanceOf), истёк окно наблюдения — отказ
     * без цепи и газа. */
    bool                live;
    tb_exec_dex_proposal_cb on_propose;
    void               *propose_user;
    tb_exec_dex_execution_cb on_execution; /* атрибуция исполнения */
    void               *execution_user;
    uint64_t            approve_cap_wei; /* рабочий лимит approve
                                          * (max_trade x MULT); 0 ->
                                          * approve ровно на ставку */
    tb_safe_cfg_t       safe;
    /* ЦЕЛОСТНОСТЬ МАРШРУТА (ревью L1): SwapRouter02 резолвит пулы
     * path СВОЕЙ фабрикой. Нога маршрута исполняется тем же пулом,
     * по которому считалась, ТОЛЬКО если фабрика venue ноги совпадает
     * с фабрикой роутера исполнения. Пусто -> фабрика Uniswap V3
     * mainnet (роутер по умолчанию — SwapRouter02). Несовпадение —
     * предложение не строится (кросс-DEX ждёт Multicall3, план R2). */
    tb_addr_t           router_factory;
} tb_exec_dex_cfg_t;

/* Инициализация заимствованного tb_exec_backend_t (не heap: структура
 * вызывающего, как у симулятора). http — заимствованный транспорт;
 * он же используется движком источников. 0 — успех. */
int tb_exec_dex_init(tb_exec_backend_t *b, tb_http_t *http,
                     const tb_exec_dex_cfg_t *cfg, tb_error_t *err);

/* Статистика адаптера (для демо и наблюдения). */
typedef struct tb_exec_dex_stats {
    uint64_t quotes;         /* котировок маршрутов (по всем хопам) */
    uint64_t rpc_calls;      /* eth_call выполнено */
    uint64_t rpc_errors;     /* ошибок RPC/транспорта */
    uint64_t submits;        /* submit-вызовов */
    uint64_t fills;          /* shadow-заполнений + live-расчётов */
    uint64_t proposals;      /* live: подготовлено предложений SafeTx */
    uint64_t supersedes;     /* live: замен живого предложения */
    uint64_t allowance_gates;/* live: trade не предложен из-за allowance */
} tb_exec_dex_stats_t;
const tb_exec_dex_stats_t *tb_exec_dex_stats(const tb_exec_backend_t *b);

/* ---- Стартерные вызовы к цепи (блокирующие; только на старте/в воркерах).
 * Транспорт и RPC берутся из venue — бэкенд не нужен. ---- */

/* Подсказка блока для котировок ЭТОГО потока: != 0 — все eth_call потока
 * котируются на указанном блоке (второй элемент params), 0 — "latest".
 * Потоковая (TLS): воркер батча пинит котировки блоком батча, поток
 * цикла (submit/monitor) котируется по latest. */
void tb_exec_dex_set_quote_block(uint64_t block);

/* eth_blockNumber через RPC venue (без пина блока). 0 — успех. */
int tb_exec_dex_block_number(tb_http_t *http, const tb_exec_dex_venue_t *v,
                             uint64_t *block, tb_error_t *err);

/* Цена газа через RPC venue: eth_gasPrice + приоритетная премия
 * (eth_maxPriorityFeePerGas; узел не отдал — премия 0, не ошибка).
 * *price_wei — сумма base+priority, как maxFeePerGas живой отправки.
 * 0 — успех; ошибка — вызывающий деградирует на конфиг-оценку. */
int tb_exec_dex_gas_price(tb_http_t *http, const tb_exec_dex_venue_t *v,
                          uint64_t *price_wei, tb_error_t *err);

/* Поиск пула на фабрике биржи: V3 — getPool(tokenA, tokenB, fee),
 * V2 — getPair(tokenA, tokenB). *found=false — фабрика не знает пары
 * (не ошибка). 0 — успех запроса. (market pool discover/inspect.) */
int tb_exec_dex_find_pool(tb_http_t *http, const tb_exec_dex_venue_t *v,
                          const tb_addr_t *token_a, const tb_addr_t *token_b,
                          uint32_t fee, bool *found, tb_addr_t *pool,
                          tb_error_t *err);

/* Проверка пула на цепи: V3 — Factory.getPool(tokenA, tokenB, fee),
 * V2 — Factory.getPair(tokenA, tokenB); адрес с цепи обязан совпасть с
 * expect_pool (конфиг — ожидание, цепь — истина).
 * 0 — совпало (или v->factory нулевой: проверка пропущена).
 * Несовпадение/пула нет — ошибка TB_EXEC_BACKEND, fail-fast старта. */
int tb_exec_dex_verify_pool(tb_http_t *http, const tb_exec_dex_venue_t *v,
                            const tb_addr_t *token_a, const tb_addr_t *token_b,
                            uint32_t fee, const tb_addr_t *expect_pool,
                            tb_error_t *err);

/* symbol()/decimals() токена с цепи (два eth_call). symbol — строка ABI
 * (в т.ч. bytes32 старых токенов), обрезается до symbol_cap-1. 0 — оба
 * донесены; ошибка — вызывающий решает (фолбэк на короткий адрес). */
int tb_exec_dex_token_info(tb_http_t *http, const tb_exec_dex_venue_t *v,
                           const tb_addr_t *token, char *symbol,
                           size_t symbol_cap, uint8_t *decimals,
                           tb_error_t *err);

#ifdef __cplusplus
}
#endif
#endif /* TB_EXEC_BACKEND_DEX_H */
