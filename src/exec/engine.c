/* ==========================================================================
 *  tb/exec/engine.c — механизм торговли: конвейер исполнения сделок.
 *
 *  Шаги цепочки и наблюдатели — см. engine.h. Реализация держит контекст
 *  ордера (exec_ctx) в таблице открытых ордеров до терминального состояния
 *  события; терминальный наблюдатель откатывает слот, пишет запись журнала
 *  и освобождает контекст.
 * ========================================================================== */
#include "tb/exec/engine.h"

#include "tb/arch/arch.h"
#include "tb/core/error_registry.h"
#include "tb/core/log.h"
#include "tb/core/marker.h"
#include "tb/core/time.h"
#include "tb/events/builtins.h"
#include "tb/exec/wallet.h"
#include "tb/sched/backoff.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ======================================================================
 *  Контекст ордера (живёт до терминала события)
 * ====================================================================== */
typedef struct exec_ctx {
    tb_exec_order_t order;      /* копия (всё — POD) */
    uint64_t        order_no;   /* порядковый номер подачи */
    uint64_t        tx_id;      /* сессия бэкенда; 0 — не отправлялся */
    tb_u256_t       quote;      /* ожидаемый выход */
    tb_u256_t       min_out;    /* фактический минимум (после авто) */
    tb_u256_t       amount_out; /* факт заполнения */
    uint32_t        polls;      /* опросов PENDING */
    bool            pending_emitted;
    bool            filled;     /* бэкенд исполнил, ждём settle/commit */
    tb_slot_t      *slot;       /* торговый слот (NULL до balance) */
} exec_ctx_t;

struct tb_exec_engine {
    tb_exec_cfg_t           cfg;
    tb_evreg_t             *reg;
    tb_wallet_t            *wallet;
    tb_exec_backend_t       backend;
    bool                    backend_borrowed; /* инъекция: destroy не наш */
    tb_ratelimit_t          rl;
    tb_breaker_t            breaker;
    tb_backoff_t            backoff;
    tb_evmod_ratelimit_t    rl_mod;  /* устойчивые контексты шагов-шлюзов */
    tb_evmod_breaker_t      br_mod;
    tb_ledger_t            *ledger;
    bool                    ledger_attached;
    struct {
        uint64_t   event_id;
        exec_ctx_t *ctx;
    } open_tb[TB_EXEC_MAX_OPEN];
    tb_exec_stats_t         stats;
    uint64_t                order_seq;
};

/* ======================================================================
 *  Мелкие помощники
 * ====================================================================== */
static exec_ctx_t *ctx_find(tb_exec_engine_t *e, uint64_t event_id)
{
    for (size_t i = 0; i < TB_EXEC_MAX_OPEN; i++)
        if (e->open_tb[i].ctx && e->open_tb[i].event_id == event_id)
            return e->open_tb[i].ctx;
    return NULL;
}

static exec_ctx_t *ctx_take(tb_exec_engine_t *e, uint64_t event_id)
{
    for (size_t i = 0; i < TB_EXEC_MAX_OPEN; i++) {
        if (e->open_tb[i].ctx && e->open_tb[i].event_id == event_id) {
            exec_ctx_t *c = e->open_tb[i].ctx;
            e->open_tb[i].ctx       = NULL;
            e->open_tb[i].event_id  = 0;
            return c;
        }
    }
    return NULL;
}

static int ctx_put(tb_exec_engine_t *e, uint64_t event_id, exec_ctx_t *c)
{
    for (size_t i = 0; i < TB_EXEC_MAX_OPEN; i++) {
        if (!e->open_tb[i].ctx) {
            e->open_tb[i].ctx      = c;
            e->open_tb[i].event_id = event_id;
            return 0;
        }
    }
    return -1;
}

/* FAIL с заполненной ошибкой события (контракт реестра). */
static tb_ev_action_t fail_with(tb_event_t *ev, uint32_t code,
                                const char *fmt, ...)
{
    char msg[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    tb_error_t err = TB_ERROR_INIT;
    tb_error_set(&err, TB_DOMAIN_USER, code, TB_LOC, "%s", msg);
    tb_event_attach(ev, &err);
    return TB_EV_ACT_FAIL;
}

static void addr_str(const tb_addr_t *a, char *buf, size_t cap)
{
    tb_addr_to_hex(a, buf, cap);
}

static void amount_str(const tb_u256_t *v, char *buf, size_t cap)
{
    tb_u256_to_dec(v, buf, cap);
}

/* Экранирование JSON-строк для записей журнала (как в builtins.c). */
static size_t json_escape(const char *in, char *out, size_t cap)
{
    size_t n = 0;
    if (!in) {
        if (cap) out[0] = '\0';
        return 0;
    }
    for (const unsigned char *p = (const unsigned char *)in; *p && n + 8 < cap; p++) {
        if (*p == '"' || *p == '\\') {
            out[n++] = '\\';
            out[n++] = (char)*p;
        } else if (*p == '\n') {
            out[n++] = '\\'; out[n++] = 'n';
        } else if (*p == '\r') {
            out[n++] = '\\'; out[n++] = 'r';
        } else if (*p == '\t') {
            out[n++] = '\\'; out[n++] = 't';
        } else if (*p < 0x20) {
            n += (size_t)snprintf(out + n, cap - n, "\\u%04x", *p);
        } else {
            out[n++] = (char)*p;
        }
    }
    out[n] = '\0';
    return n;
}

/* ======================================================================
 *  Записи журнала (конверт-схема: SUCCESS / PENDING / ERROR)
 * ====================================================================== */
static void record_trade(tb_exec_engine_t *e, tb_event_t *ev, exec_ctx_t *c,
                         const char *type, int status,
                         const tb_u256_t *out, const tb_error_t *err,
                         uint32_t attempt, int64_t next_at)
{
    if (!e->ledger)
        return;

    char in_hex[43], out_hex[43];
    addr_str(&c->order.token_in, in_hex, sizeof(in_hex));
    addr_str(&c->order.token_out, out_hex, sizeof(out_hex));

    char amt_in[80], amt_min[80], amt_out[80], amt_quote[80];
    tb_u256_t zero = tb_u256_from_u64(0);
    amount_str(&c->order.amount_in, amt_in, sizeof(amt_in));
    amount_str(&c->min_out, amt_min, sizeof(amt_min));
    amount_str(&c->quote, amt_quote, sizeof(amt_quote));
    amount_str(out ? out : &zero, amt_out, sizeof(amt_out));

    char payload[1600];
    int n = -1;

    if (status == 0) { /* SUCCESS */
        n = snprintf(payload, sizeof(payload),
                     "{\"schema_version\":\"1.0\",\"source\":\"%s\","
                     "\"operation\":\"swap_exact_in\",\"status\":\"SUCCESS\","
                     "\"request_id\":%llu,\"timestamp\":%llu,"
                     "\"data\":{\"order\":%llu,\"token_in\":\"%s\","
                     "\"token_out\":\"%s\",\"amount_in\":\"%s\","
                     "\"amount_out_min\":\"%s\",\"amount_out\":\"%s\","
                     "\"quote\":\"%s\",\"hops\":%zu,\"attempt\":%u}}",
                     e->backend.name, (unsigned long long)ev->id,
                     (unsigned long long)tb_wall_ms(),
                     (unsigned long long)c->order_no, in_hex, out_hex,
                     amt_in, amt_min, amt_out, amt_quote,
                     tb_route_hops(&c->order.route), attempt);
    } else if (status == 2) { /* PENDING */
        n = snprintf(payload, sizeof(payload),
                     "{\"schema_version\":\"1.0\",\"source\":\"%s\","
                     "\"operation\":\"swap_exact_in\",\"status\":\"PENDING\","
                     "\"request_id\":%llu,\"timestamp\":%llu,"
                     "\"data\":{\"order\":%llu,\"token_in\":\"%s\","
                     "\"token_out\":\"%s\",\"amount_in\":\"%s\","
                     "\"amount_out_min\":\"%s\",\"quote\":\"%s\","
                     "\"hops\":%zu,\"attempt\":%u},"
                     "\"retry\":{\"attempt\":%u,\"next_attempt_at\":%lld}}",
                     e->backend.name, (unsigned long long)ev->id,
                     (unsigned long long)tb_wall_ms(),
                     (unsigned long long)c->order_no, in_hex, out_hex,
                     amt_in, amt_min, amt_quote,
                     tb_route_hops(&c->order.route), attempt,
                     attempt, (long long)next_at);
    } else { /* ERROR */
        const tb_error_meta_t *meta =
            tb_registry_meta(err ? err->domain : 0, err ? err->code : 0);
        char msg_esc[2 * TB_ERROR_MSG_MAX];
        json_escape(err ? err->message : "", msg_esc, sizeof(msg_esc));
        n = snprintf(payload, sizeof(payload),
                     "{\"schema_version\":\"1.0\",\"source\":\"%s\","
                     "\"operation\":\"swap_exact_in\",\"status\":\"ERROR\","
                     "\"request_id\":%llu,\"timestamp\":%llu,\"data\":null,"
                     "\"error\":{\"code\":\"%s\",\"domain\":\"%s\","
                     "\"severity\":\"%s\",\"retryable\":%s,"
                     "\"retry_after_ms\":%u,\"message\":\"%s\"}}",
                     e->backend.name, (unsigned long long)ev->id,
                     (unsigned long long)tb_wall_ms(),
                     meta ? meta->name : "UNKNOWN",
                     tb_domain_name(err ? err->domain : 0),
                     tb_severity_name((tb_severity_t)(err ? err->severity : 0)),
                     (err && err->retryable) ? "true" : "false",
                     err ? err->retry_after_ms : 0, msg_esc);
    }
    if (n < 0 || (size_t)n >= sizeof(payload))
        return;

    char uuid[64];
    if (status == 2)
        snprintf(uuid, sizeof(uuid), "%s:%llu:%u", type,
                 (unsigned long long)ev->id, attempt);
    else
        snprintf(uuid, sizeof(uuid), "%s:%llu", type,
                 (unsigned long long)ev->id);

    tb_error_t lerr = TB_ERROR_INIT;
    if (tb_ledger_emit(e->ledger, type, payload, uuid, NULL, &lerr) != 0)
        TB_LOG_ERROR("exec: журнал отклонил запись %s (ордер %llu): %s",
                     type, (unsigned long long)c->order_no, lerr.message);
}

/* ======================================================================
 *  Шаги конвейера
 * ====================================================================== */

/* ---- preflight: валидация ордера и риск-лимиты ---- */
static tb_ev_action_t step_preflight(tb_evreg_t *r, tb_event_t *ev, void *user)
{
    tb_exec_engine_t *e = user;
    exec_ctx_t *c = ctx_find(e, ev->id);
    if (!c)
        return fail_with(ev, TB_EXEC_CAPACITY, "контекст ордера потерян");

    const tb_exec_order_t *o = &c->order;

    /* Каждый проход цепочки начинается с чистой ошибкой: повтор после
     * retryable-отказа не должен доносить прошлую ошибку в терминал. */
    tb_error_t zero = TB_ERROR_INIT;
    tb_event_attach(ev, &zero);

    if (tb_u256_is_zero(&o->amount_in))
        return fail_with(ev, TB_EXEC_ZERO_AMOUNT, "сумма входа равна нулю");

    uint64_t now = tb_wall_ms();
    if (now > o->deadline_ms)
        return fail_with(ev, TB_EXEC_DEADLINE_PAST,
                         "дедлайн %llu в прошлом (сейчас %llu)",
                         (unsigned long long)o->deadline_ms,
                         (unsigned long long)now);
    if (o->deadline_ms - now < e->cfg.min_deadline_ahead_ms)
        return fail_with(ev, TB_EXEC_DEADLINE_PAST,
                         "дедлайн через %llu мс — меньше запаса %llu мс",
                         (unsigned long long)(o->deadline_ms - now),
                         (unsigned long long)e->cfg.min_deadline_ahead_ms);

    if (!tb_route_valid(&o->route))
        return fail_with(ev, TB_EXEC_INVALID_PATH,
                         "маршрут не является путём V3 (len=%u)",
                         o->route.len);

    /* Согласованность маршрута и ордера: платим нативным — маршрут
     * начинается с wrapped (как ethIn => path[0] == WETH в контракте). */
    tb_addr_t expect_in = tb_addr_is_native(&o->token_in)
                              ? e->cfg.wrapped_native
                              : o->token_in;
    tb_addr_t expect_out = tb_addr_is_native(&o->token_out)
                               ? e->cfg.wrapped_native
                               : o->token_out;
    tb_addr_t route_first = tb_route_first(&o->route);
    tb_addr_t route_last  = tb_route_last(&o->route);
    if (!tb_addr_eq(&route_first, &expect_in))
        return fail_with(ev, TB_EXEC_ROUTE_MISMATCH,
                         "маршрут должен начинаться с входного токена "
                         "(нативный вход — с wrapped)");
    if (!tb_addr_eq(&route_last, &expect_out))
        return fail_with(ev, TB_EXEC_ROUTE_MISMATCH,
                         "маршрут должен заканчиваться выходным токеном "
                         "(нативный выход — wrapped)");

    size_t hops = tb_route_hops(&o->route);
    if (hops > e->cfg.max_hops)
        return fail_with(ev, TB_EXEC_HOPS_LIMIT,
                         "хопов %zu больше лимита %u", hops,
                         e->cfg.max_hops);

    if (!tb_u256_is_zero(&e->cfg.max_amount_in) &&
        tb_u256_cmp(&o->amount_in, &e->cfg.max_amount_in) > 0)
        return fail_with(ev, TB_EXEC_NOTIONAL_LIMIT,
                         "сумма ордера выше лимита");

    if (e->cfg.allow_n > 0) {
        for (size_t i = 0; i <= hops; i++) {
            tb_addr_t tok = tb_route_token(&o->route, i);
            if (tb_addr_is_native(&tok))
                continue;
            bool ok = false;
            for (uint32_t k = 0; k < e->cfg.allow_n; k++)
                if (tb_addr_eq(&tok, &e->cfg.allow[k])) { ok = true; break; }
            if (!ok)
                return fail_with(ev, TB_EXEC_ALLOWLIST,
                                 "токен позиции %zu вне разрешённого списка",
                                 i);
        }
    }

    char in_hex[43], out_hex[43];
    addr_str(&o->token_in, in_hex, sizeof(in_hex));
    addr_str(&o->token_out, out_hex, sizeof(out_hex));
    tb_event_step(r, ev,
                  "ордер %llu: %s -> %s, хопов %zu, дедлайн через %llu мс — "
                  "проверки пройдены",
                  (unsigned long long)c->order_no, in_hex, out_hex, hops,
                  (unsigned long long)(o->deadline_ms - now));
    return TB_EV_ACT_CHAIN;
}

/* ---- quote: ожидаемый выход и минимум ---- */
static tb_ev_action_t step_quote(tb_evreg_t *r, tb_event_t *ev, void *user)
{
    tb_exec_engine_t *e = user;
    exec_ctx_t *c = ctx_find(e, ev->id);
    if (!c)
        return fail_with(ev, TB_EXEC_CAPACITY, "контекст ордера потерян");

    /* Повтор цепочки (поллинг PENDING) не должен менять котировку и
     * минимум: условия зафиксированы в момент отправки ордера. */
    if (c->tx_id != 0) {
        tb_event_step(r, ev, "котировка и минимум зафиксированы при отправке");
        return TB_EV_ACT_CHAIN;
    }

    tb_error_t err = TB_ERROR_INIT;
    if (e->backend.quote(&e->backend, &c->order.route, &c->order.amount_in,
                         tb_now_ms(), &c->quote, &err) != 0) {
        tb_event_attach(ev, &err);
        return TB_EV_ACT_FAIL;
    }

    if (!tb_u256_is_zero(&c->order.amount_out_min)) {
        c->min_out = c->order.amount_out_min;
        char mins[80], qs[80];
        amount_str(&c->min_out, mins, sizeof(mins));
        amount_str(&c->quote, qs, sizeof(qs));
        tb_event_step(r, ev, "котировка %s; минимум задан ордером: %s%s",
                      qs, mins,
                      tb_u256_cmp(&c->min_out, &c->quote) > 0
                          ? " (выше котировки — высокий риск отказа)"
                          : "");
    } else {
        /* Авто-минимум от политики проскальзывания: quote * (1 - bp/10000). */
        tb_u256_t m = c->quote;
        if (!tb_u256_mul_u64(&m, &m, 10000 - e->cfg.max_slippage_bp))
            return fail_with(ev, TB_EXEC_OVERFLOW,
                             "переполнение при расчёте авто-минимума");
        /* div_u64 возвращает остаток: ненулевой остаток — усечение вниз
         * (копейки), а не ошибка. */
        (void)tb_u256_div_u64(&m, &m, 10000);
        c->min_out = m;
        char mins[80], qs[80];
        amount_str(&c->min_out, mins, sizeof(mins));
        amount_str(&c->quote, qs, sizeof(qs));
        tb_event_step(r, ev,
                      "котировка %s; авто-минимум (%u б.п.): %s",
                      qs, e->cfg.max_slippage_bp, mins);
    }
    return TB_EV_ACT_CHAIN;
}

/* ---- balance: слот, списание входа, wrap нативного ---- */
static tb_ev_action_t step_balance(tb_evreg_t *r, tb_event_t *ev, void *user)
{
    tb_exec_engine_t *e = user;
    exec_ctx_t *c = ctx_find(e, ev->id);
    if (!c)
        return fail_with(ev, TB_EXEC_CAPACITY, "контекст ордера потерян");

    /* Полный повтор цепочки (лимит/предохранитель/backoff) не должен
     * списывать вход дважды: слот уже открыт — резервы стоят. */
    if (c->slot) {
        tb_event_step(r, ev, "слот уже открыт (повтор цепочки) — вход зарезервирован");
        return TB_EV_ACT_CHAIN;
    }

    tb_error_t err = TB_ERROR_INIT;
    c->slot = tb_slot_open(e->wallet, &err);
    if (!c->slot) {
        tb_event_attach(ev, &err);
        return TB_EV_ACT_FAIL;
    }

    char amt[80];
    amount_str(&c->order.amount_in, amt, sizeof(amt));

    if (tb_addr_is_native(&c->order.token_in)) {
        /* Аналог _receiveInput: deposit заворачивает нативный в wrapped,
         * и вход ЦЕЛИКОМ уходит маршрутизатору (контракт не держит WETH).
         * Итог по слоту: native -amount, wrapped +amount -amount = 0. */
        if (tb_slot_wrap(c->slot, &e->cfg.wrapped_native,
                         &c->order.amount_in, &err) != 0) {
            tb_slot_rollback(c->slot);
            c->slot = NULL;
            tb_event_attach(ev, &err);
            return TB_EV_ACT_FAIL;
        }
        if (tb_slot_debit(c->slot, &e->cfg.wrapped_native,
                          &c->order.amount_in, &err) != 0) {
            tb_slot_rollback(c->slot);
            c->slot = NULL;
            tb_event_attach(ev, &err);
            return TB_EV_ACT_FAIL;
        }
        char whex[43];
        addr_str(&e->cfg.wrapped_native, whex, sizeof(whex));
        tb_event_step(r, ev, "нативный вход %s завёрнут в %s (deposit) "
                             "и передан маршрутизатору",
                      amt, whex);
    } else {
        if (tb_slot_debit(c->slot, &c->order.token_in, &c->order.amount_in,
                          &err) != 0) {
            tb_slot_rollback(c->slot);
            c->slot = NULL;
            tb_event_attach(ev, &err);
            return TB_EV_ACT_FAIL;
        }
        tb_u256_t avail = tb_slot_available(c->slot, &c->order.token_in);
        char av[80];
        amount_str(&avail, av, sizeof(av));
        tb_event_step(r, ev, "вход %s списан в слот (доступно было %s)",
                      amt, av);
    }
    return TB_EV_ACT_CHAIN;
}

/* ---- approve: allowance «роутеру» ровно на сумму входа ---- */
static tb_ev_action_t step_approve(tb_evreg_t *r, tb_event_t *ev, void *user)
{
    tb_exec_engine_t *e = user;
    exec_ctx_t *c = ctx_find(e, ev->id);
    if (!c)
        return fail_with(ev, TB_EXEC_CAPACITY, "контекст ордера потерян");
    if (!c->slot)
        return fail_with(ev, TB_EXEC_VALUE_MISMATCH,
                         "approve без открытого слота (инвариант конвейера)");

    tb_addr_t real_in = tb_addr_is_native(&c->order.token_in)
                            ? e->cfg.wrapped_native
                            : c->order.token_in;
    tb_error_t err = TB_ERROR_INIT;
    if (tb_slot_approve(c->slot, &real_in, &c->order.amount_in, &err) != 0) {
        tb_event_attach(ev, &err);
        return TB_EV_ACT_FAIL;
    }
    char amt[80];
    amount_str(&c->order.amount_in, amt, sizeof(amt));
    tb_event_step(r, ev, "allowance маршрутизатору: %s (сброс в 0 после расчёта)",
                  amt);
    return TB_EV_ACT_CHAIN;
}

/* ---- execute: отправка / поллинг бэкенда ---- */
static tb_ev_action_t step_execute(tb_evreg_t *r, tb_event_t *ev, void *user)
{
    tb_exec_engine_t *e = user;
    exec_ctx_t *c = ctx_find(e, ev->id);
    if (!c)
        return fail_with(ev, TB_EXEC_CAPACITY, "контекст ордера потерян");
    if (!c->slot)
        return fail_with(ev, TB_EXEC_VALUE_MISMATCH,
                         "execute без открытого слота (инвариант конвейера)");

    if (tb_wall_ms() > c->order.deadline_ms)
        return fail_with(ev, TB_EXEC_DEADLINE_PAST,
                         "дедлайн истёк к моменту отправки");

    uint64_t now = tb_now_ms();
    tb_error_t err = TB_ERROR_INIT;
    tb_u256_t out;

    if (c->tx_id == 0) {
        int rc = e->backend.submit(&e->backend, &c->order.route,
                                   &c->order.amount_in, &c->min_out,
                                   c->order.deadline_ms, now, &c->tx_id, &out,
                                   &err);
        if (rc == 0) {
            c->amount_out = out;
            c->filled     = true;
            tb_breaker_success(&e->breaker);
            char amt[80];
            amount_str(&out, amt, sizeof(amt));
            tb_event_step(r, ev, "исполнено сразу: выход %s", amt);
            return TB_EV_ACT_CHAIN;
        }
        if (rc == 1) {
            /* PENDING: сессия принята, результат — поллингом. Поллинг — это
             * ПОЛНЫЙ ПОВТОР цепочки (tb_event_retry_at): на wake шлюзы
             * перепроверятся, balance увидит открытый слот и не спишет
             * дважды, а execute по tx_id перейдёт к poll. */
            if (!c->pending_emitted) {
                c->pending_emitted = true;
                e->stats.pending_records++;
                record_trade(e, ev, c, "trade.submitted", 2, NULL, NULL,
                             ev->attempts,
                             (int64_t)(tb_wall_ms() + e->cfg.fill_poll_interval_ms));
            }
            tb_event_step(r, ev,
                          "принято в работу (сессия %llu) — опрос через %llu мс",
                          (unsigned long long)c->tx_id,
                          (unsigned long long)e->cfg.fill_poll_interval_ms);
            tb_event_retry_at(ev, now + e->cfg.fill_poll_interval_ms);
            return TB_EV_ACT_BLOCK;
        }
        /* Отказ. Retryable (серверные) — backoff и полный повтор цепочки;
         * шлюзы перепроверятся, слот уже открыт и не спишет дважды. */
        if (err.retryable) {
            e->stats.retries++;
            tb_breaker_failure(&e->breaker, now);
            uint64_t delay = tb_backoff_delay(&e->backoff, ev->attempts, &err);
            tb_event_attach(ev, &err);
            tb_event_step(r, ev,
                          "отказ бэкенда (%s), повторяемая — backoff %llu мс, "
                          "сбоев подряд %u/%u",
                          err.message, (unsigned long long)delay,
                          e->breaker.consecutive_failures,
                          e->breaker.failure_threshold);
            tb_event_retry_at(ev, now + delay);
            return TB_EV_ACT_BLOCK;
        }
        tb_event_attach(ev, &err);
        tb_event_step(r, ev, "отказ бэкенда: %s", err.message);
        return TB_EV_ACT_FAIL;
    }

    /* Поллинг принятой сессии. */
    int rc = e->backend.poll(&e->backend, c->tx_id, now, &out, &err);
    if (rc == 0) {
        c->amount_out = out;
        c->filled     = true;
        tb_breaker_success(&e->breaker);
        char amt[80];
        amount_str(&out, amt, sizeof(amt));
        tb_event_step(r, ev, "заполнено после %u опросов: выход %s",
                      c->polls, amt);
        return TB_EV_ACT_CHAIN;
    }
    if (rc == 1) {
        c->polls++;
        if (c->polls >= e->cfg.fill_poll_max)
            return fail_with(ev, TB_EXEC_TIMEOUT,
                             "PENDING не заполнился за %u опросов",
                             c->polls);
        tb_event_step(r, ev, "ещё в работе (опрос %u/%u) — следующий через %llu мс",
                      c->polls, e->cfg.fill_poll_max,
                      (unsigned long long)e->cfg.fill_poll_interval_ms);
        /* Полный повтор: на wake execute снова зайдёт в poll по tx_id. */
        tb_event_retry_at(ev, now + e->cfg.fill_poll_interval_ms);
        return TB_EV_ACT_BLOCK;
    }
    tb_event_attach(ev, &err);
    tb_event_step(r, ev, "сессия %llu отклонена: %s",
                  (unsigned long long)c->tx_id, err.message);
    return TB_EV_ACT_FAIL;
}

/* ---- settle: зачисление выхода, unwrap нативного, сброс allowance ---- */
static tb_ev_action_t step_settle(tb_evreg_t *r, tb_event_t *ev, void *user)
{
    tb_exec_engine_t *e = user;
    exec_ctx_t *c = ctx_find(e, ev->id);
    if (!c)
        return fail_with(ev, TB_EXEC_CAPACITY, "контекст ордера потерян");
    if (!c->filled)
        return fail_with(ev, TB_EXEC_VALUE_MISMATCH,
                         "settle без заполнения (инвариант конвейера)");

    /* Защитное сличение (роутер уже проверял — это инвариант, не политика). */
    if (tb_u256_cmp(&c->amount_out, &c->min_out) < 0)
        return fail_with(ev, TB_EXEC_SLIPPAGE,
                         "выход ниже минимума на расчёте (инвариант)");

    tb_error_t err = TB_ERROR_INIT;
    tb_addr_t real_out = tb_addr_is_native(&c->order.token_out)
                             ? e->cfg.wrapped_native
                             : c->order.token_out;

    if (tb_slot_credit(c->slot, &real_out, &c->amount_out, &err) != 0) {
        tb_event_attach(ev, &err);
        return TB_EV_ACT_FAIL;
    }
    if (tb_addr_is_native(&c->order.token_out)) {
        /* Аналог _unwrapAndSend: withdraw + перевод вызывающему. */
        if (tb_slot_unwrap(c->slot, &e->cfg.wrapped_native, &c->amount_out,
                           &err) != 0) {
            tb_event_attach(ev, &err);
            return TB_EV_ACT_FAIL;
        }
    }

    /* Сброс allowance после расчёта — forceApprove(router, 0). */
    tb_addr_t real_in = tb_addr_is_native(&c->order.token_in)
                            ? e->cfg.wrapped_native
                            : c->order.token_in;
    tb_u256_t zero = tb_u256_from_u64(0);
    if (tb_slot_approve(c->slot, &real_in, &zero, &err) != 0) {
        tb_event_attach(ev, &err);
        return TB_EV_ACT_FAIL;
    }

    char amt[80];
    amount_str(&c->amount_out, amt, sizeof(amt));
    tb_event_step(r, ev, "расчёт: выход %s%s, allowance сброшен в 0", amt,
                  tb_addr_is_native(&c->order.token_out)
                      ? " (разворачивается в нативный и переводится)"
                      : "");
    return TB_EV_ACT_CHAIN;
}

/* ---- commit: атомарная фиксация слота и запись успеха ---- */
static tb_ev_action_t step_commit(tb_evreg_t *r, tb_event_t *ev, void *user)
{
    tb_exec_engine_t *e = user;
    exec_ctx_t *c = ctx_find(e, ev->id);
    if (!c)
        return fail_with(ev, TB_EXEC_CAPACITY, "контекст ордера потерян");

    tb_error_t err = TB_ERROR_INIT;
    if (tb_slot_commit(c->slot, &err) != 0) {
        /* Слот не закрыт — откатываем, чтобы не утек. */
        tb_slot_rollback(c->slot);
        c->slot = NULL;
        tb_error_wrap(&err, TB_LOC, "фиксация слота ордера %llu",
                      (unsigned long long)c->order_no);
        tb_event_attach(ev, &err);
        return TB_EV_ACT_FAIL;
    }
    c->slot = NULL;

    e->stats.filled++;
    record_trade(e, ev, c, "trade.executed", 0, &c->amount_out, NULL,
                 ev->attempts, -1);
    tb_event_step(r, ev, "слот зафиксирован: балансы применены атомарно");
    return TB_EV_ACT_CHAIN; /* конец цепочки — неявный DONE */
}

/* ======================================================================
 *  Терминальный наблюдатель: откат слота, запись отказа, уборка
 * ====================================================================== */
static void exec_observer(tb_evreg_t *r, tb_event_t *ev, void *user)
{
    tb_exec_engine_t *e = user;
    (void)r;

    exec_ctx_t *c = ctx_take(e, ev->id);
    if (!c)
        return;

    switch (ev->state) {
    case TB_EV_DONE:
        /* trade.executed уже записан шагом commit. */
        break;

    case TB_EV_FAILED: {
        if (c->slot) {
            tb_slot_rollback(c->slot); /* revert: журнал мутаций отброшен */
            c->slot = NULL;
        }
        e->stats.failed++;
        switch (ev->err.code) {
        case TB_EXEC_SLIPPAGE:            e->stats.slippage_rejects++; break;
        case TB_EXEC_TIMEOUT:             e->stats.timeouts++;         break;
        case TB_EXEC_INSUFFICIENT_LIQUIDITY:
        case TB_EXEC_BACKEND:             e->stats.backend_rejects++;  break;
        default: break;
        }
        record_trade(e, ev, c, "trade.failed", 1, NULL, &ev->err,
                     ev->attempts, -1);
        break;
    }

    case TB_EV_CANCELED: {
        if (c->slot) {
            tb_slot_rollback(c->slot);
            c->slot = NULL;
        }
        e->stats.canceled++;
        tb_error_t cerr = TB_ERROR_INIT;
        tb_error_set(&cerr, TB_DOMAIN_API, TB_API_CANCELED, TB_LOC,
                     "ордер отменён владельцем до исполнения");
        record_trade(e, ev, c, "trade.canceled", 1, NULL, &cerr,
                     ev->attempts, -1);
        break;
    }

    default:
        break;
    }

    free(c);
}

/* ======================================================================
 *  Жизненный цикл движка
 * ====================================================================== */
tb_exec_engine_t *tb_exec_create(const tb_exec_cfg_t *cfg, tb_error_t *err)
{
    if (!cfg || tb_addr_is_native(&cfg->wrapped_native)) {
        TB_ERROR_SET(err, TB_DOMAIN_CORE, TB_CORE_INVALID_ARG,
                     "движок: wrapped_native обязателен (ненулевой адрес)");
        return NULL;
    }

    tb_exec_engine_t *e = calloc(1, sizeof(*e));
    if (!e) {
        TB_ERROR_SET(err, TB_DOMAIN_CORE, TB_CORE_NO_MEMORY,
                     "движок: нет памяти");
        return NULL;
    }
    e->cfg = *cfg;

    /* Значения по умолчанию (нулевые поля конфигурации). */
    if (e->cfg.max_hops == 0)              e->cfg.max_hops = TB_EXEC_MAX_HOPS;
    if (e->cfg.max_slippage_bp == 0)       e->cfg.max_slippage_bp = 50;
    if (e->cfg.min_deadline_ahead_ms == 0) e->cfg.min_deadline_ahead_ms = 500;
    if (e->cfg.rl_rate_per_min == 0)       e->cfg.rl_rate_per_min = 30;
    if (e->cfg.rl_burst == 0)              e->cfg.rl_burst = 8;
    if (e->cfg.br_threshold == 0)          e->cfg.br_threshold = 3;
    if (e->cfg.br_cooldown_ms == 0)        e->cfg.br_cooldown_ms = 3000;
    if (e->cfg.backoff_base_ms == 0)       e->cfg.backoff_base_ms = 700;
    if (e->cfg.backoff_cap_ms == 0)        e->cfg.backoff_cap_ms = 5000;
    if (e->cfg.backoff_jitter < 0)         e->cfg.backoff_jitter = 0.25;
    if (e->cfg.fill_poll_max == 0)         e->cfg.fill_poll_max = 8;
    if (e->cfg.fill_poll_interval_ms == 0) e->cfg.fill_poll_interval_ms = 500;
    if (e->cfg.seed == 0)                  e->cfg.seed = tb_rdtsc();

    e->reg    = tb_evreg_create(0, err);
    e->wallet = tb_wallet_create(err);
    if (!e->reg || !e->wallet) {
        tb_exec_destroy(e);
        return NULL;
    }
    if (e->cfg.backend && e->cfg.script == NULL) {
        /* Инъекция (v0.3): живой DEX-адаптер; заимствован, не разрушаем. */
        e->backend = *e->cfg.backend;
        e->backend_borrowed = true;
    } else if (tb_exec_sim_init(&e->backend, e->cfg.script, e->cfg.script_n,
                                e->cfg.seed, err) != 0) {
        tb_exec_destroy(e);
        return NULL;
    }

    tb_ratelimit_init(&e->rl, e->cfg.rl_rate_per_min, e->cfg.rl_burst);
    tb_breaker_init(&e->breaker, e->cfg.br_threshold, e->cfg.br_cooldown_ms);
    tb_backoff_init(&e->backoff, e->cfg.backoff_base_ms, e->cfg.backoff_cap_ms,
                    e->cfg.backoff_jitter, e->cfg.seed ^ 0x5eed);

    e->rl_mod.rl = &e->rl;
    e->br_mod.br = &e->breaker;

    /* Цепочка исполнения (порядок = порядок регистрации). */
    struct { const char *name; tb_ev_handler_t fn; void *user; } steps[] = {
        { "preflight", step_preflight, e },
        { "ratelimit", tb_evmod_ratelimit_fn, &e->rl_mod },
        { "breaker",   tb_evmod_breaker_fn,   &e->br_mod },
        { "quote",     step_quote,     e },
        { "balance",   step_balance,   e },
        { "approve",   step_approve,   e },
        { "execute",   step_execute,   e },
        { "settle",    step_settle,    e },
        { "commit",    step_commit,    e },
    };
    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        if (tb_evreg_add(e->reg, steps[i].name, steps[i].fn, steps[i].user,
                         err) < 0) {
            tb_exec_destroy(e);
            return NULL;
        }
    }

    /* Терминальный наблюдатель — ПЕРВЫЙ (до attach журнала): успевает
     * откатить слот и записать trade.* до event.trace. */
    if (tb_evreg_on_state(e->reg, TB_EV_TERMINAL_MASK, exec_observer, e) < 0) {
        TB_ERROR_SET(err, TB_DOMAIN_EVENTS, TB_EV_NO_SLOT,
                     "движок: не удалось подписать терминального наблюдателя");
        tb_exec_destroy(e);
        return NULL;
    }
    return e;
}

void tb_exec_destroy(tb_exec_engine_t *e)
{
    if (!e)
        return;
    /* Живые контексты (не дошедшие до терминала) — откатить и убрать. */
    for (size_t i = 0; i < TB_EXEC_MAX_OPEN; i++) {
        exec_ctx_t *c = e->open_tb[i].ctx;
        if (!c)
            continue;
        if (c->slot) {
            tb_slot_rollback(c->slot);
            c->slot = NULL;
        }
        free(c);
        e->open_tb[i].ctx      = NULL;
        e->open_tb[i].event_id = 0;
    }
    if (e->backend.destroy && !e->backend_borrowed)
        e->backend.destroy(&e->backend);
    tb_wallet_destroy(e->wallet);
    tb_evreg_destroy(e->reg);
    free(e);
}

tb_evreg_t *tb_exec_registry(tb_exec_engine_t *e)
{
    return e ? e->reg : NULL;
}

struct tb_wallet *tb_exec_wallet(tb_exec_engine_t *e)
{
    return e ? e->wallet : NULL;
}

int tb_exec_attach_ledger(tb_exec_engine_t *e, tb_ledger_t *ledger)
{
    if (!e || !ledger)
        return -1;
    e->ledger         = ledger;
    e->ledger_attached = true;
    return tb_ev_ledger_attach(e->reg, ledger);
}

int tb_exec_quote(tb_exec_engine_t *e, const tb_route_t *route,
                  const tb_u256_t *amount_in, tb_u256_t *out, tb_error_t *err)
{
    if (!e || !route || !amount_in || !out)
        TB_ERROR_FAIL(err, TB_DOMAIN_CORE, TB_CORE_INVALID_ARG,
                      "котировка: пустой аргумент");
    return e->backend.quote(&e->backend, route, amount_in, tb_now_ms(), out,
                            err);
}

uint64_t tb_exec_submit(tb_exec_engine_t *e, const tb_exec_order_t *order,
                        tb_error_t *err)
{
    if (!e || !order) {
        TB_ERROR_SET(err, TB_DOMAIN_CORE, TB_CORE_INVALID_ARG,
                     "подача: пустой движок/ордер");
        return 0;
    }

    exec_ctx_t *c = calloc(1, sizeof(*c));
    if (!c) {
        TB_ERROR_SET(err, TB_DOMAIN_CORE, TB_CORE_NO_MEMORY,
                     "подача: нет памяти на контекст ордера");
        return 0;
    }
    c->order   = *order;
    c->order_no = ++e->order_seq;

    tb_event_t *ev = tb_event_create(e->reg, TB_EXEC_KIND_SWAP, 0, err);
    if (!ev) {
        free(c);
        return 0;
    }
    ev->tag = c->order_no;

    if (ctx_put(e, ev->id, c) != 0) {
        tb_event_close(e->reg, ev);
        free(c);
        TB_ERROR_SET(err, TB_DOMAIN_USER, TB_EXEC_CAPACITY,
                     "таблица открытых ордеров полна (%d)", TB_EXEC_MAX_OPEN);
        return 0;
    }

    e->stats.submitted++;
    TB_MARK("exec: ордер %llu подан (событие #%llu)",
            (unsigned long long)c->order_no, (unsigned long long)ev->id);

    if (tb_evreg_dispatch(e->reg, ev, err) != 0) {
        /* Ошибка механизма: контекст мог уже уйти с терминалом — проверяем. */
        exec_ctx_t *live = ctx_take(e, ev->id);
        if (live) {
            if (live->slot)
                tb_slot_rollback(live->slot);
            free(live);
        }
        return 0;
    }
    return ev->id;
}

const tb_exec_stats_t *tb_exec_stats(const tb_exec_engine_t *e)
{
    return e ? &e->stats : NULL;
}
