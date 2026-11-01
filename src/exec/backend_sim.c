#define TB_CURRENT_GROUP TB_G_SIM /* зона ответственности файла */
/* ==========================================================================
 *  tb/exec/backend_sim.c — детерминированный симулятор DEX-бэкенда.
 *
 *  Котировки СИНТЕТИЧЕСКИЕ: «цена» токена — константа, выведенная из
 *  crc32c его адреса (и сида), в фиксированной точке 1e6 на отрезке
 *  [0.5, 1.5). Никакой связи с рынком нет — это стендалон для конвейера
 *  исполнения, пока не приехал живой HTTP-адаптер (итерация «живые данные»).
 *
 *  Модель стоимости маршрута (за хоп): обмен по отношению цен токенов,
 *  минус fee тира, минус «ценовое влияние» маршрута. Заполнение —
 *  котировка, умноженная на фактор сценария (997/1000 = 0.3% хуже).
 * ========================================================================== */
#include "tb/exec/backend.h"

#include "tb/arch/arch.h"

#include <stdlib.h>
#include <string.h>

#define TB_SIM_MAX_TX 32
#define TB_SIM_PRICE_SCALE 1000000u /* 1e6: фиксированная точка цен */

typedef struct {
    uint64_t  id;
    uint64_t  fill_at_ms;
    tb_u256_t out;
    tb_u256_t min_out;
    bool      in_use;
} tb_sim_tx_t;

typedef struct {
    uint64_t                      seed;
    const tb_exec_sim_scenario_t *script;
    uint32_t                      script_n;
    uint32_t                      pos;
    tb_sim_tx_t                   txs[TB_SIM_MAX_TX];
    uint64_t                      next_tx;
} tb_exec_sim_t;

/* ---- Цена токена: crc32c(адрес, сид) -> [0.5, 1.5) в 1e6 ---- */
static uint32_t sim_price_fp6(const tb_exec_sim_t *sim, const tb_addr_t *tok)
{
    uint8_t buf[20 + 8];
    memcpy(buf, tok->b, 20);
    memcpy(buf + 20, &sim->seed, 8); /* сид влияет на «рынок» целиком */
    uint32_t h = tb_crc32c(0, buf, sizeof(buf));
    return TB_SIM_PRICE_SCALE / 2 + h % (TB_SIM_PRICE_SCALE / 2);
}

/* Котировка маршрута: обмен цен + fee тиров + ценовое влияние. */
static int sim_quote(tb_exec_backend_t *b, const tb_route_t *route,
                     const tb_u256_t *amount_in, uint64_t now_ms,
                     tb_u256_t *out, tb_error_t *err)
{
    (void)now_ms;
    tb_exec_sim_t *sim = b->ctx;
    if (!tb_route_valid(route))
        TB_ERROR_FAIL(err, TB_DOMAIN_USER, TB_EXEC_INVALID_PATH,
                      "симулятор: невалидный маршрут");
    if (tb_u256_is_zero(amount_in))
        TB_ERROR_FAIL(err, TB_DOMAIN_USER, TB_EXEC_ZERO_AMOUNT,
                      "симулятор: нулевая сумма входа");

    tb_u256_t v = *amount_in;
    size_t hops = tb_route_hops(route);
    for (size_t hop = 0; hop < hops; hop++) {
        tb_addr_t a = tb_route_token(route, hop);
        tb_addr_t bb = tb_route_token(route, hop + 1);
        uint32_t fee = tb_route_fee(route, hop);

        uint32_t pa = sim_price_fp6(sim, &a);
        uint32_t pb = sim_price_fp6(sim, &bb);
        if (!tb_u256_mul_u64(&v, &v, pa) || !tb_u256_div_u64(&v, &v, pb))
            TB_ERROR_FAIL(err, TB_DOMAIN_USER, TB_EXEC_OVERFLOW,
                          "симулятор: переполнение котировки (хоп %zu)", hop);
        if (!tb_u256_mul_u64(&v, &v, TB_SIM_PRICE_SCALE - fee) ||
            !tb_u256_div_u64(&v, &v, TB_SIM_PRICE_SCALE))
            TB_ERROR_FAIL(err, TB_DOMAIN_USER, TB_EXEC_OVERFLOW,
                          "симулятор: переполнение fee (хоп %zu)", hop);
    }

    /* Ценовое влияние маршрута: 1/2000..1/5000 от выхода (0.05%..0.02%). */
    uint32_t impact_div = 2000u + tb_crc32c(0, route->bytes, route->len) % 3000u;
    tb_u256_t impact;
    if (!tb_u256_div_u64(&impact, &v, impact_div) ||
        !tb_u256_sub(&v, &v, &impact))
        TB_ERROR_FAIL(err, TB_DOMAIN_USER, TB_EXEC_OVERFLOW,
                      "симулятор: переполнение влияния");

    *out = v;
    return 0;
}

static const tb_exec_sim_scenario_t *sim_next_scenario(tb_exec_sim_t *sim)
{
    static const tb_exec_sim_scenario_t def = TB_SIM_DEFAULT_FILL;
    if (!sim->script || sim->script_n == 0)
        return &def;
    return &sim->script[sim->pos++ % sim->script_n];
}

static void sim_reject(tb_error_t *err, uint32_t code, bool retryable,
                       const char *note)
{
    TB_ERROR_SET(err, TB_DOMAIN_USER, code, "%s", note ? note : "отказ DEX");
    err->retryable = retryable;
}

static tb_sim_tx_t *sim_tx_slot(tb_exec_sim_t *sim, uint64_t tx_id)
{
    if (tx_id == 0)
        return NULL;
    for (size_t i = 0; i < TB_SIM_MAX_TX; i++)
        if (sim->txs[i].in_use && sim->txs[i].id == tx_id)
            return &sim->txs[i];
    return NULL;
}

static int sim_submit(tb_exec_backend_t *b, const tb_route_t *route,
                      const tb_u256_t *amount_in, const tb_u256_t *min_out,
                      uint64_t deadline_ms, uint64_t now_ms,
                      uint64_t *tx_id, tb_u256_t *amount_out, tb_error_t *err)
{
    (void)deadline_ms;
    tb_exec_sim_t *sim = b->ctx;

    tb_u256_t quote;
    if (sim_quote(b, route, amount_in, now_ms, &quote, err) != 0)
        return -1;

    const tb_exec_sim_scenario_t *sc = sim_next_scenario(sim);
    tb_u256_t out = quote;
    if (sc->out_den != 0 && (sc->out_num != sc->out_den)) {
        if (!tb_u256_mul_u64(&out, &quote, sc->out_num) ||
            !tb_u256_div_u64(&out, &out, sc->out_den))
            TB_ERROR_FAIL(err, TB_DOMAIN_USER, TB_EXEC_OVERFLOW,
                          "симулятор: переполнение фактора заполнения");
    }

    switch (sc->outcome) {
    case TB_SIM_FILL:
        /* Сличение минимумов — на стороне «роутера», как amountOutMinimum. */
        if (min_out && tb_u256_cmp(&out, min_out) < 0) {
            sim_reject(err, TB_EXEC_SLIPPAGE, false,
                       "выход ниже amountOutMinimum — роутер ревертит (slippage)");
            return -1;
        }
        if (amount_out)
            *amount_out = out;
        if (tx_id)
            *tx_id = 0;
        return 0;

    case TB_SIM_SUBMIT: {
        tb_sim_tx_t *slot = NULL;
        for (size_t i = 0; i < TB_SIM_MAX_TX; i++)
            if (!sim->txs[i].in_use) { slot = &sim->txs[i]; break; }
        if (!slot)
            TB_ERROR_FAIL(err, TB_DOMAIN_USER, TB_EXEC_CAPACITY,
                          "симулятор: таблица сессий полна");
        slot->in_use     = true;
        slot->id         = ++sim->next_tx;
        slot->fill_at_ms = now_ms + sc->fill_delay_ms;
        slot->out        = out;
        slot->min_out    = min_out ? *min_out : tb_u256_from_u64(0);
        if (tx_id)
            *tx_id = slot->id;
        return 1;
    }

    case TB_SIM_REJECT:
    default:
        sim_reject(err, sc->code, sc->retryable, sc->note);
        return -1;
    }
}

static int sim_poll(tb_exec_backend_t *b, uint64_t tx_id, uint64_t now_ms,
                    tb_u256_t *amount_out, tb_error_t *err)
{
    tb_exec_sim_t *sim = b->ctx;
    tb_sim_tx_t *tx = sim_tx_slot(sim, tx_id);
    if (!tx)
        TB_ERROR_FAIL(err, TB_DOMAIN_USER, TB_EXEC_BACKEND,
                      "симулятор: неизвестная сессия исполнения");

    if (now_ms < tx->fill_at_ms)
        return 1; /* ещё в работе */

    if (!tb_u256_is_zero(&tx->min_out) &&
        tb_u256_cmp(&tx->out, &tx->min_out) < 0) {
        tx->in_use = false;
        sim_reject(err, TB_EXEC_SLIPPAGE, false,
                   "заполнение ниже amountOutMinimum — сессия ревертнута");
        return -1;
    }
    if (amount_out)
        *amount_out = tx->out;
    tx->in_use = false;
    return 0;
}

static void sim_destroy(tb_exec_backend_t *b)
{
    if (b && b->ctx)
        free(b->ctx);
}

int tb_exec_sim_init(tb_exec_backend_t *b, const tb_exec_sim_scenario_t *script,
                     uint32_t script_n, uint64_t seed, tb_error_t *err)
{
    if (!b)
        TB_ERROR_FAIL(err, TB_DOMAIN_CORE, TB_CORE_INVALID_ARG,
                      "симулятор: пустой backend");
    tb_exec_sim_t *sim = calloc(1, sizeof(*sim));
    if (!sim)
        TB_ERROR_FAIL(err, TB_DOMAIN_CORE, TB_CORE_NO_MEMORY,
                      "симулятор: нет памяти");
    sim->seed     = seed;
    sim->script   = script;
    sim->script_n = script_n;

    memset(b, 0, sizeof(*b));
    b->name    = "sim-dex";
    b->ctx     = sim;
    b->destroy = sim_destroy;
    b->quote   = sim_quote;
    b->submit  = sim_submit;
    b->poll    = sim_poll;
    return 0;
}
