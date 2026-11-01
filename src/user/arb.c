#define TB_CURRENT_GROUP TB_G_ARB /* зона ответственности файла */
/* ==========================================================================
 *  tb/user/arb.c — рантайм конвейера арбитража (итерации 3+5 роадмапа).
 *
 *  Цикл опроса (период poll_ms, самоперепланирование как в bot_demo):
 *    1) сбор снапшотов: на каждый пул watchlist — три котировки QuoterV2
 *       (probe-покупка, большая покупка, продажа назад) в воркере пула
 *       потоков; слот снапшота фиксирован индексом пула — гонок нет;
 *    2) фильтры 1–4 (чистые) в воркере фазы решения;
 *    3) Decision Engine в том же воркере: цепные котировки, поиск
 *       минимальной выгодной ставки до верха контракта;
 *    4) продолжение в цикле: запись decision.evaluated (атрибуция
 *       источников и вердиктов) и tb_exec_submit выгодных циклов;
 *    5) монитор отмены: пока ордер PENDING, перекотировка цикла тем же
 *       размером; продажа ниже min_out (безубыток) — tb_event_cancel:
 *       движок откатывает слот, теряем только моделируемый газ
 *       (decision.canceled + stats.cancel_fee_wei).
 *
 *  Потоковая модель: состояние — в потоке-владельце цикла; воркеры
 *  получают готовые входы, результаты — через done. Владение памятью
 *  job'ов — у arg_dtor пула (единственный владелец).
 * ========================================================================== */
#include "tb/user/arb.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tb/core/time.h"
#include "tb/ledger/ledger.h"
#include "tb/sched/loop.h"
#include "tb/sched/threadpool.h"

#define TB_ARB_MAX_PENDING 8
#define TB_ARB_MAX_RESULTS 16

typedef struct tb_arb {
    tb_arb_cfg_t         cfg;
    tb_exec_engine_t    *engine;   /* заимствован */
    tb_ledger_t         *ledger;   /* заимствован */
    tb_http_t           *http;     /* заимствован */
    tb_exec_backend_t    dexb;     /* DEX-адаптер котировок */
    tb_loop_t           *loop;
    bool                 running;
    uint64_t             poll_seq;
    tb_arb_stats_t       stats;

    tb_arb_hist_t        hist[TB_ARB_MAX_POOLS];

    /* последний решённый батч котировок — для веб-страниц (пишет и читает
     * только поток цикла); копия делается в decide_done до free батча */
    tb_arb_snap_t        last_snaps[TB_ARB_MAX_POOLS];
    size_t               last_n;
    uint64_t             last_seq;

    struct pending_ent {
        uint64_t   event_id;
        tb_u256_t  size;     /* вход ордера — размер перекотировки */
        tb_u256_t  min_out;  /* безубыток */
        tb_route_t route;    /* цикл: покупка + продажа */
    } pending[TB_ARB_MAX_PENDING];
    size_t pending_n;
} tb_arb_t;

/* ==========================================================================
 *  Котировка через DEX-адаптер (блокирующая — только из воркеров)
 * ========================================================================== */

static int arb_quote_dir(tb_arb_t *a, const tb_addr_t *pool, uint32_t fee,
                         const tb_addr_t *tin, const tb_addr_t *tout,
                         const tb_u256_t *amount, tb_u256_t *out,
                         tb_error_t *err)
{
    tb_route_t r = tb_route_single(tin, tout, fee, err);
    tb_route_set_pool(&r, 0, pool);
    return a->dexb.quote(&a->dexb, &r, amount, tb_now_ms(), out, err);
}

/* ==========================================================================
 *  Фаза 1: батч снапшотов
 * ========================================================================== */

typedef struct arb_batch {
    tb_arb_t       *a;
    uint64_t        seq;
    size_t          remaining;      /* атомика не нужна: меняем в цикле */
    size_t          n;              /* = pools_n, слоты фиксированы */
    tb_arb_snap_t   snaps[TB_ARB_MAX_POOLS];
} arb_batch_t;

void batch_maybe_decide(tb_arb_t *a, arb_batch_t *b);

typedef struct pool_job {
    tb_arb_t     *a;
    arb_batch_t  *batch;  /* владение у decide-цепочки */
    size_t        idx;
} pool_job_t;

static void pool_work(void *arg)
{
    pool_job_t *j = arg;
    tb_arb_t *a = j->a;
    const tb_arb_pool_t *p = &a->cfg.pools[j->idx];
    tb_arb_snap_t *s = &j->batch->snaps[j->idx]; /* слот фиксирован */
    tb_error_t err = TB_ERROR_INIT;

    memset(s, 0, sizeof(*s));
    s->pool_idx = (uint32_t)j->idx;
    s->pool = p->pool;
    s->token = p->token;
    s->fee = p->fee;
    snprintf(s->dex, sizeof(s->dex), "%s", p->dex);
    s->ts_ms = tb_now_ms();

    tb_u256_t probe =
        tb_u256_from_u64(a->cfg.probe_wei ? a->cfg.probe_wei
                                          : 100000000000000ull);
    tb_u256_t big = probe;
    uint64_t mult =
        a->cfg.liquidity_probe_mult ? a->cfg.liquidity_probe_mult : 10;
    (void)tb_u256_mul_u64(&big, &big, mult);

    if (arb_quote_dir(a, &p->pool, p->fee, &a->cfg.weth, &p->token, &probe,
                      &s->buy_out, &err) != 0)
        return; /* пустой снапшот отсеется фильтром актуальности */
    if (arb_quote_dir(a, &p->pool, p->fee, &a->cfg.weth, &p->token, &big,
                      &s->big_out, &err) != 0)
        return;
    if (arb_quote_dir(a, &p->pool, p->fee, &p->token, &a->cfg.weth,
                      &s->buy_out, &s->sell_back, &err) == 0)
        s->complete = true; /* всё донесено: фильтрам можно верить */
}

static void pool_done(tb_loop_t *loop, void *arg)
{
    (void)loop;
    pool_job_t *j = arg;
    if (j->batch->remaining > 0)
        j->batch->remaining--;
    batch_maybe_decide(j->a, j->batch);
}

static void pool_dtor(void *arg)
{
    free(arg);
}

void batch_maybe_decide(tb_arb_t *a, arb_batch_t *b);

/* ==========================================================================
 *  Фаза 2+3: фильтры + Decision Engine (воркер)
 * ========================================================================== */

typedef struct decide_job {
    tb_arb_t         *a;
    arb_batch_t      *batch;
    tb_arb_verdict_t  verdicts[TB_ARB_MAX_POOLS];
    size_t            results_n;
    struct res_ent {
        tb_arb_pair_res_t r;
        size_t buy_i, sell_i;
    } results[TB_ARB_MAX_RESULTS];
} decide_job_t;

/* Котировщик для tb_arb_eval_pair: пул ищется в watchlist по адресу. */
static int decide_quote(void *ctx, const tb_addr_t *pool, uint32_t fee,
                        const tb_addr_t *tin, const tb_addr_t *tout,
                        const tb_u256_t *amount, tb_u256_t *out,
                        tb_error_t *err)
{
    tb_arb_t *a = ctx;
    for (uint32_t i = 0; i < a->cfg.pools_n; i++)
        if (tb_addr_eq(&a->cfg.pools[i].pool, pool))
            return arb_quote_dir(a, pool, fee, tin, tout, amount, out, err);
    TB_ERROR_SET(err, TB_DOMAIN_USER, TB_EXEC_BACKEND,
                 "decide: пул не из watchlist");
    return -1;
}

static void decide_work(void *arg)
{
    decide_job_t *d = arg;
    tb_arb_t *a = d->a;
    uint64_t now = tb_now_ms();

    if (getenv("TB_ARB_DEBUG"))
        for (size_t i = 0; i < d->batch->n; i++) {
            char b1[80], b2[80], b3[80];
            tb_u256_to_dec(&d->batch->snaps[i].buy_out, b1, sizeof(b1));
            tb_u256_to_dec(&d->batch->snaps[i].big_out, b2, sizeof(b2));
            tb_u256_to_dec(&d->batch->snaps[i].sell_back, b3, sizeof(b3));
            fprintf(stderr,
                    "[arb] snap %zu: buy=%s (%08llx %08llx) big=%s back=%s\n",
                    i, b1,
                    (unsigned long long)d->batch->snaps[i].buy_out.w[1],
                    (unsigned long long)d->batch->snaps[i].buy_out.w[0],
                    b2, b3);
        }
    for (size_t i = 0; i < d->batch->n; i++) {
        tb_arb_filter_eval(&a->cfg, &d->batch->snaps[i],
                           &a->hist[d->batch->snaps[i].pool_idx], now,
                           &d->verdicts[i]);
        if (d->verdicts[i].pass)
            tb_arb_hist_push(&a->hist[d->batch->snaps[i].pool_idx],
                             &d->batch->snaps[i].buy_out);
        else
            a->stats.filter_rejects++;
    }

    for (size_t i = 0; i < d->batch->n && d->results_n < TB_ARB_MAX_RESULTS;
         i++) {
        if (!d->verdicts[i].pass)
            continue;
        for (size_t j = 0;
             j < d->batch->n && d->results_n < TB_ARB_MAX_RESULTS; j++) {
            if (i == j || !d->verdicts[j].pass)
                continue;
            if (!tb_addr_eq(&d->batch->snaps[i].token,
                            &d->batch->snaps[j].token))
                continue;

            tb_arb_pair_in_t pin = {
                .buy = &d->batch->snaps[i],
                .sell = &d->batch->snaps[j],
            };
            struct res_ent *e = &d->results[d->results_n];
            tb_error_t qerr = TB_ERROR_INIT;
            int rc = tb_arb_eval_pair(&a->cfg, &pin, decide_quote, a, &e->r,
                                      &qerr);
            if (rc < 0) {
                /* не глотаем: причина и развёрнутые детали — в запись */
                a->stats.quote_errors++;
                e->buy_i = i;
                e->sell_i = j;
                snprintf(e->r.reason, sizeof(e->r.reason),
                         "ошибка котировщика: %s", qerr.message);
                if (qerr.details_n) {
                    char det[384];
                    if (tb_error_format_details(&qerr, det, sizeof(det)) > 0) {
                        size_t off = strlen(e->r.reason);
                        snprintf(e->r.reason + off,
                                 sizeof(e->r.reason) - off, " | %s", det);
                    }
                }
                d->results_n++;
                continue;
            }
            e->buy_i = i;
            e->sell_i = j;
            d->results_n++;
            a->stats.pairs_evaluated++;
            if (e->r.ok)
                a->stats.pairs_profitable++;
        }
    }
}

static void decide_done(tb_loop_t *loop, void *arg);

static void decide_dtor(void *arg)
{
    decide_job_t *d = arg;
    free(d->batch);
    free(d);
}

/* ==========================================================================
 *  Журнал: decision.evaluated / decision.submitted / decision.canceled
 * ========================================================================== */

static void emit_evaluated(tb_arb_t *a, arb_batch_t *b, decide_job_t *d)
{
    char payload[4096];
    size_t n = 0;

#define APPEND(...)                                                        \
    do {                                                                   \
        n += (size_t)snprintf(payload + n, sizeof(payload) - n, __VA_ARGS__); \
        if (n >= sizeof(payload))                                          \
            n = sizeof(payload) - 1;                                       \
    } while (0)

    APPEND("{\"poll\":%llu,\"snapshots\":[", (unsigned long long)b->seq);
    for (size_t i = 0; i < b->n; i++) {
        char ph[43];
        tb_addr_to_hex(&b->snaps[i].pool, ph, sizeof(ph));
        APPEND("%s{\"pool\":\"%s\",\"dex\":\"%s\",\"age_ms\":%llu,"
               "\"impact_bp\":%u,\"vol_bp\":%u,\"pass\":%s,\"reject\":\"%s\"}",
               i ? "," : "", ph, b->snaps[i].dex,
               (unsigned long long)d->verdicts[i].age_ms,
               d->verdicts[i].impact_bp, d->verdicts[i].vol_bp,
               d->verdicts[i].pass ? "true" : "false",
               d->verdicts[i].pass ? "" : d->verdicts[i].reject);
    }
    APPEND("],\"pairs\":[");
    for (size_t k = 0; k < d->results_n; k++) {
        struct res_ent *e = &d->results[k];
        char bp[43], sp[43], sz[80], so[80];
        tb_addr_to_hex(&b->snaps[e->buy_i].pool, bp, sizeof(bp));
        tb_addr_to_hex(&b->snaps[e->sell_i].pool, sp, sizeof(sp));
        tb_u256_to_dec(&e->r.size, sz, sizeof(sz));
        tb_u256_to_dec(&e->r.sell_out, so, sizeof(so));
        APPEND("%s{\"buy\":\"%s\",\"sell\":\"%s\",\"ok\":%s,"
               "\"size\":\"%s\",\"sell_out\":\"%s\",\"probes\":%u,"
               "\"reason\":\"%s\"}",
               k ? "," : "", bp, sp, e->r.ok ? "true" : "false", sz, so,
               e->r.probes, e->r.reason);
    }
    APPEND("]}");
#undef APPEND

    tb_error_t lerr = TB_ERROR_INIT;
    tb_ledger_emit(a->ledger, "decision.evaluated", payload, NULL, NULL,
                   &lerr);
}

/* ==========================================================================
 *  Фаза 4: подача ордеров (поток цикла)
 * ========================================================================== */

static void decide_done(tb_loop_t *loop, void *arg)
{
    (void)loop;
    decide_job_t *d = arg;
    tb_arb_t *a = d->a;

    /* веб-страницы: сохранить последние котировки до освобождения батча */
    if (d->batch->seq > a->last_seq) {
        memcpy(a->last_snaps, d->batch->snaps,
               sizeof(tb_arb_snap_t) * d->batch->n);
        a->last_n = d->batch->n;
        a->last_seq = d->batch->seq;
    }

    emit_evaluated(a, d->batch, d);

    uint32_t budget =
        a->cfg.max_pairs_per_poll ? a->cfg.max_pairs_per_poll : 1;
    for (size_t k = 0; k < d->results_n && budget > 0; k++) {
        struct res_ent *e = &d->results[k];
        if (!e->r.ok || a->pending_n >= TB_ARB_MAX_PENDING)
            continue;
        budget--;

        tb_arb_snap_t *bs = &d->batch->snaps[e->buy_i];
        tb_arb_snap_t *ss = &d->batch->snaps[e->sell_i];
        tb_addr_t toks[3] = { a->cfg.weth, bs->token, a->cfg.weth };
        uint32_t fees[2] = { bs->fee, ss->fee };

        tb_error_t rerr = TB_ERROR_INIT;
        tb_route_t route = tb_route_encode(toks, fees, 3, &rerr);
        tb_route_set_pool(&route, 0, &bs->pool);
        tb_route_set_pool(&route, 1, &ss->pool);
        if (rerr.code != 0)
            continue;

        tb_exec_order_t o = {0};
        o.token_in = a->cfg.weth; /* цикл целиком в wrapped-токенах */
        o.token_out = a->cfg.weth;
        o.amount_in = e->r.size;
        o.amount_out_min = e->r.min_out;
        o.deadline_ms =
            tb_wall_ms() + (a->cfg.deadline_ms ? a->cfg.deadline_ms : 8000);
        o.route = route;

        tb_error_t serr = TB_ERROR_INIT;
        uint64_t ev = tb_exec_submit(a->engine, &o, &serr);
        if (ev == 0)
            continue;

        a->stats.orders_submitted++;
        a->pending[a->pending_n].event_id = ev;
        a->pending[a->pending_n].size = e->r.size;
        a->pending[a->pending_n].min_out = e->r.min_out;
        a->pending[a->pending_n].route = route;
        a->pending_n++;

        char sz[80], mo[80];
        tb_u256_to_dec(&e->r.size, sz, sizeof(sz));
        tb_u256_to_dec(&e->r.min_out, mo, sizeof(mo));
        char pay[512];
        snprintf(pay, sizeof(pay),
                 "{\"event_id\":%llu,\"size\":\"%s\",\"min_out\":\"%s\"}",
                 (unsigned long long)ev, sz, mo);
        tb_error_t lerr = TB_ERROR_INIT;
        tb_ledger_emit(a->ledger, "decision.submitted", pay, NULL, NULL,
                       &lerr);
    }
}

void batch_maybe_decide(tb_arb_t *a, arb_batch_t *b)
{
    if (b->remaining > 0)
        return;
    a->stats.quotes += b->n;

    decide_job_t *d = calloc(1, sizeof(*d));
    if (!d) {
        free(b);
        return;
    }
    d->a = a;
    d->batch = b;

    tb_pool_submit_t s = {
        .job = decide_work,
        .arg = d,
        .arg_dtor = decide_dtor,
        .loop = a->loop,
        .done_prio = TB_PRIO_HIGH,
        .done = decide_done,
    };
    if (!tb_threadpool_submit(tb_loop_pool(a->loop), &s)) {
        free(b);
        free(d);
    }
    /* успешный submit: владение у пула — batch освободится в decide_dtor */
}

/* ==========================================================================
 *  Опрос
 * ========================================================================== */

static void arb_poll(tb_loop_t *loop, struct tb_task *task, void *arg);

static void schedule_poll(tb_arb_t *a)
{
    if (!a->running)
        return;
    tb_loop_call_later(a->loop, a->cfg.poll_ms ? a->cfg.poll_ms : 8000,
                       TB_PRIO_NORMAL, "arb.poll", arb_poll, a, NULL);
}

static void arb_poll(tb_loop_t *loop, struct tb_task *task, void *arg)
{
    (void)task;
    tb_arb_t *a = arg;
    schedule_poll(a); /* следующий цикл — сразу, чтобы не потерять период */

    a->stats.polls++;
    a->poll_seq++;

    arb_batch_t *b = calloc(1, sizeof(*b));
    if (!b)
        return;
    b->a = a;
    b->seq = a->poll_seq;
    b->n = a->cfg.pools_n;

    for (uint32_t i = 0; i < a->cfg.pools_n; i++) {
        pool_job_t *j = calloc(1, sizeof(*j));
        if (!j)
            continue;
        j->a = a;
        j->batch = b;
        j->idx = i;
        b->remaining++;

        tb_pool_submit_t s = {
            .job = pool_work,
            .arg = j,
            .arg_dtor = pool_dtor,
            .loop = loop,
            .done_prio = TB_PRIO_HIGH,
            .done = pool_done,
        };
        if (!tb_threadpool_submit(tb_loop_pool(loop), &s)) {
            b->remaining--;
            free(j);
        }
    }
    if (b->remaining == 0)
        batch_maybe_decide(a, b);
    /* иначе batch освободится цепочкой pool_done -> decide_dtor */
}

/* ==========================================================================
 *  Фаза 5: монитор отмены невыгодных PENDING-ордеров
 * ========================================================================== */

typedef struct mon_job {
    tb_arb_t  *a;
    size_t     idx;      /* индекс в a->pending (на момент постановки) */
    uint64_t   event_id;
    tb_u256_t  size;
    tb_u256_t  min_out;
    tb_route_t route;
    int        rc;       /* 0 — котировка есть */
    tb_u256_t  sell_out;
} mon_job_t;

static void mon_work(void *arg)
{
    mon_job_t *m = arg;
    tb_arb_t *a = m->a;
    tb_error_t err = TB_ERROR_INIT;
    m->rc = a->dexb.quote(&a->dexb, &m->route, &m->size, tb_now_ms(),
                          &m->sell_out, &err);
}

static void mon_done(tb_loop_t *loop, void *arg)
{
    (void)loop;
    mon_job_t *m = arg;
    tb_arb_t *a = m->a;

    /* проверить, что ордер ещё жив и это тот же элемент */
    size_t idx = a->pending_n;
    for (size_t i = 0; i < a->pending_n; i++)
        if (a->pending[i].event_id == m->event_id) {
            idx = i;
            break;
        }
    if (idx == a->pending_n)
        return; /* уже погашен/заменён — просто выходим */

    if (m->rc != 0)
        return; /* котировка не получена — не отменяем вслепую */

    if (tb_u256_cmp(&m->sell_out, &m->min_out) >= 0)
        return; /* ещё выгодно (порог — безубыток ордера) */

    tb_event_t *ev =
        tb_evreg_find(tb_exec_registry(a->engine), m->event_id);
    if (!ev || ev->state != TB_EV_BLOCKED)
        return;

    if (!tb_event_cancel(tb_exec_registry(a->engine), ev))
        return;

    a->stats.orders_canceled++;
    uint64_t gas = a->cfg.gas_est_wei ? a->cfg.gas_est_wei
                                      : 200000000000000ull;
    a->stats.cancel_fee_wei += gas;

    char so[80], mo[80], pay[320];
    tb_u256_to_dec(&m->sell_out, so, sizeof(so));
    tb_u256_to_dec(&m->min_out, mo, sizeof(mo));
    snprintf(pay, sizeof(pay),
             "{\"event_id\":%llu,\"sell_now\":\"%s\",\"min_out\":\"%s\","
             "\"fee_lost_wei\":%llu,\"reason\":\"цикл стал невыгодным\"}",
             (unsigned long long)m->event_id, so, mo,
             (unsigned long long)gas);
    tb_error_t lerr = TB_ERROR_INIT;
    tb_ledger_emit(a->ledger, "decision.canceled", pay, NULL, NULL, &lerr);

    /* из наблюдения уберёт следующий проход монитора (терминал) */
}

static void mon_dtor(void *arg)
{
    free(arg);
}

static void arb_monitor(tb_loop_t *loop, struct tb_task *task, void *arg)
{
    (void)task;
    tb_arb_t *a = arg;

    for (size_t i = 0; i < a->pending_n;) {
        tb_event_t *ev =
            tb_evreg_find(tb_exec_registry(a->engine),
                          a->pending[i].event_id);
        if (!ev || ev->state != TB_EV_BLOCKED) {
            memmove(&a->pending[i], &a->pending[i + 1],
                    (a->pending_n - i - 1) * sizeof(a->pending[0]));
            a->pending_n--;
            continue; /* терминал — убрать из наблюдения */
        }
        i++;
    }

    for (size_t i = 0; i < a->pending_n; i++) {
        mon_job_t *m = calloc(1, sizeof(*m));
        if (!m)
            continue;
        m->a = a;
        m->idx = i;
        m->event_id = a->pending[i].event_id;
        m->size = a->pending[i].size;
        m->min_out = a->pending[i].min_out;
        m->route = a->pending[i].route;

        tb_pool_submit_t s = {
            .job = mon_work,
            .arg = m,
            .arg_dtor = mon_dtor,
            .loop = loop,
            .done_prio = TB_PRIO_HIGH,
            .done = mon_done,
        };
        if (!tb_threadpool_submit(tb_loop_pool(loop), &s))
            free(m);
    }
}

/* ==========================================================================
 *  Lifecycle
 * ========================================================================== */

tb_arb_t *tb_arb_create(const tb_arb_cfg_t *cfg, tb_exec_engine_t *engine,
                        tb_ledger_t *ledger, tb_http_t *http,
                        tb_error_t *err)
{
    if (!cfg || !engine || !http) {
        TB_ERROR_SET(err, TB_DOMAIN_CORE, TB_CORE_INVALID_ARG, "arb: args");
        return NULL;
    }

    tb_arb_t *a = calloc(1, sizeof(*a));
    if (!a) {
        TB_ERROR_SET(err, TB_DOMAIN_CORE, TB_CORE_NO_MEMORY, "arb");
        return NULL;
    }
    a->cfg = *cfg;
    tb_arb_defaults(&a->cfg);
    a->engine = engine;
    a->ledger = ledger;
    a->http = http;

    if (tb_exec_dex_init(&a->dexb, http, &a->cfg.dex, err) != 0) {
        free(a);
        return NULL;
    }
    return a;
}

void tb_arb_destroy(tb_arb_t *a)
{
    if (!a)
        return;
    tb_arb_stop(a);
    if (a->dexb.destroy)
        a->dexb.destroy(&a->dexb);
    free(a);
}

int tb_arb_start(tb_arb_t *a, struct tb_loop *loop, tb_error_t *err)
{
    if (!a || !loop) {
        TB_ERROR_SET(err, TB_DOMAIN_CORE, TB_CORE_INVALID_ARG, "arb: start");
        return -1;
    }
    if (a->running)
        return 0;
    a->loop = loop;
    a->running = true;

    /* первый опрос сразу, дальше — самоперепланирование */
    tb_loop_call_later(loop, 0, TB_PRIO_NORMAL, "arb.poll.first", arb_poll,
                       a, NULL);
    tb_loop_call_every(loop, a->cfg.poll_ms ? a->cfg.poll_ms : 8000,
                       TB_PRIO_LOW, "arb.monitor", arb_monitor, a, NULL);
    return 0;
}

void tb_arb_stop(tb_arb_t *a)
{
    if (!a || !a->running)
        return;
    a->running = false;
    /* отмена задач — из потока цикла; при остановке цикла задачи и так
     * дренируются (владелец вызывает stop до destroy цикла) */
    a->loop = NULL;
}

const tb_arb_stats_t *tb_arb_stats(const tb_arb_t *a)
{
    return a ? &a->stats : NULL;
}

/* Статистика RPC собственного цитировщика модуля (опрашивает пулы). */
const tb_exec_dex_stats_t *tb_arb_quote_stats(const tb_arb_t *a)
{
    return a ? tb_exec_dex_stats(&a->dexb) : NULL;
}

/* ---- данные для веб-страниц: только из потока цикла ---- */

size_t tb_arb_pools_n(const tb_arb_t *a)
{
    return a ? a->cfg.pools_n : 0;
}

const tb_arb_pool_t *tb_arb_pool(const tb_arb_t *a, size_t i)
{
    if (!a || i >= a->cfg.pools_n)
        return NULL;
    return &a->cfg.pools[i];
}

const tb_arb_snap_t *tb_arb_last_snap(const tb_arb_t *a, size_t i)
{
    if (!a || i >= a->last_n)
        return NULL;
    return &a->last_snaps[i];
}

uint64_t tb_arb_probe_wei(const tb_arb_t *a)
{
    return a ? a->cfg.probe_wei : 0;
}
