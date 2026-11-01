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
#define TB_DEX_POOLS_PER_VENUE 8
#define TB_DEX_NAME_MAX 24
#define TB_DEX_MAX_SESSIONS 32

/* ---- Биржа (venue): RPC + quoter + её пулы ---- */
typedef struct tb_exec_dex_venue {
    char       name[TB_DEX_NAME_MAX];          /* "uniswap-v3" — в записях */
    char       rpc_url[TB_HTTP_URL_MAX];       /* JSON-RPC endpoint (HTTPS) */
    char       rpc_host_pinned[TB_HTTP_HOST_MAX]; /* непусто: IP для
                                                     соединения, Host/SNI
                                                     из rpc_url */
    tb_addr_t  quoter;                         /* адрес QuoterV2 биржи */
    uint32_t   timeout_ms;                     /* 0 -> 8000 */

    tb_addr_t  pools[TB_DEX_POOLS_PER_VENUE];  /* пулы биржи: хоп с таким
                                                  пулом исполняется здесь */
    uint32_t   pools_n;
} tb_exec_dex_venue_t;

typedef struct tb_exec_dex_cfg {
    tb_exec_dex_venue_t venues[TB_DEX_MAX_VENUES];
    uint32_t            venues_n;
    /* shadow-заполнение: задержка в [min..max] мс (джиттер от seed) */
    uint32_t            fill_delay_min_ms;  /* 0 -> 300 */
    uint32_t            fill_delay_max_ms;  /* 0 -> 900 */
    uint64_t            seed;               /* 0 -> tb_rdtsc() */

    /* live: исполнение из Safe через execTransaction (v0.4). Котировки
     * те же (QuoterV2), submit/poll — настоящие транзакции; фактический
     * выход = дельта balanceOf(Safe). false -> shadow (по умолчанию). */
    bool                live;
    tb_safe_cfg_t       safe;
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
    uint64_t fills;          /* shadow-заполнений */
} tb_exec_dex_stats_t;
const tb_exec_dex_stats_t *tb_exec_dex_stats(const tb_exec_backend_t *b);

#ifdef __cplusplus
}
#endif
#endif /* TB_EXEC_BACKEND_DEX_H */
