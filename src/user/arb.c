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
#include <time.h> /* nanosleep: ожидание временного пула старта */

#include "tb/core/time.h"
#include "tb/ledger/ledger.h"
#include "tb/sched/loop.h"
#include "tb/sched/threadpool.h"

#define TB_ARB_MAX_PENDING 8
#define TB_ARB_MAX_RESULTS 64 /* пар на батч; 16 молча резало
                               * мультичейн-батчи (до 77 пар) */
#define TB_ARB_START_THREADS 6 /* параллельные стартерные eth_call */

typedef struct tb_arb {
    tb_arb_cfg_t         cfg;
    tb_exec_engine_t    *engine;   /* заимствован */
    tb_ledger_t         *ledger;   /* заимствован */
    tb_http_t           *http;     /* заимствован */
    tb_exec_backend_t    dexb;     /* DEX-адаптер котировок */
    tb_loop_t           *loop;
    bool                 running;
    bool                 stopping; /* атомарно: воркеры сворачиваются
                                     * между котировками — остановка не
                                     * ждёт очередь блокирующих RPC */
    uint64_t             poll_seq;
    tb_arb_stats_t       stats;

    tb_arb_hist_t        hist[TB_ARB_MAX_POOLS];

    /* кэш токенов с цепи (В5): symbol/decimals по адресу пула; пустой
     * символ — цепь не ответила, страница покажет короткий адрес */
    struct tok_info {
        tb_addr_t addr;
        char      symbol[16];
        uint8_t   decimals;
    } toks[TB_ARB_MAX_POOLS];
    uint32_t             toks_n;

    /* последний решённый батч котировок — для веб-страниц (пишет и читает
     * только поток цикла); копия делается в decide_done до free батча */
    tb_arb_snap_t        last_snaps[TB_ARB_MAX_POOLS];
    size_t               last_n;
    uint64_t             last_seq;
    tb_arb_route_quote_t last_quote; /* В7: реальная котировка лучшей пары */

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
    /* мультичейн: свой блок на каждую цепь батча (номер блока одной
     * цепи не существует на другой); 0 — не достали. Газ той же цепи —
     * цена base+priority (0 — не достали: фолбэк gas_est_wei) */
    uint64_t        chains[TB_DEX_MAX_VENUES];
    uint64_t        blocks[TB_DEX_MAX_VENUES];
    uint64_t        gas_wei[TB_DEX_MAX_VENUES];
    uint32_t        chains_n;
    tb_arb_snap_t   snaps[TB_ARB_MAX_POOLS];
    /* Владение батчём — подсчёт ссылок: pool_jobs_n pool_dtor-ов + один
     * слот решающей цепочки (decide_dtor либо отказ submit в
     * batch_maybe_decide). Остановка цикла рвёт done-постинги — без
     * этого батч утекал. Атомики: отклонённый постинг зовёт dtor из
     * воркера, успешные — из цикла. */
    size_t          pool_jobs_n;    /* подано pool_job (до первого submit) */
    size_t          releases;       /* batch_release исполнено */
    bool            pools_submitted;/* submit_pool_jobs отработал (prep) */
    bool            decided;        /* batch_maybe_decide уже позван */
} arb_batch_t;

static void batch_release(arb_batch_t *b)
{
    size_t r = __atomic_fetch_add(&b->releases, 1, __ATOMIC_ACQ_REL) + 1;
    /* Решили (decided) — счёт до pool_jobs_n + 1: последний release
     * сделает решающая цепочка. Не решили (remaining застрял > 0 —
     * остановка цикла отклонила done-постинги) — decide-слот никто не
     * займёт, последнего пул-релиза достаточно. Все done исполняются
     * раньше своих dtor-ов на потоке цикла, поэтому к моменту
     * r == pool_jobs_n флаг decided уже виден; воркерские dtor-ы бывают
     * только при отклонённых постингах, т.е. когда decided не взведён. */
    bool decided = __atomic_load_n(&b->decided, __ATOMIC_ACQUIRE);
    size_t total =
        __atomic_load_n(&b->pool_jobs_n, __ATOMIC_ACQUIRE) + (decided ? 1 : 0);
    if (r == total)
        free(b);
}

void batch_maybe_decide(tb_arb_t *a, arb_batch_t *b);

typedef struct pool_job {
    tb_arb_t     *a;
    arb_batch_t  *batch;  /* владение у decide-цепочки */
    size_t        idx;
} pool_job_t;

static const struct tok_info *tok_find(const tb_arb_t *a, const tb_addr_t *t)
{
    for (uint32_t i = 0; i < a->toks_n; i++)
        if (tb_addr_eq(&a->toks[i].addr, t))
            return &a->toks[i];
    return NULL;
}

static void pool_work(void *arg)
{
    pool_job_t *j = arg;
    tb_arb_t *a = j->a;
    const tb_arb_pool_t *p = &a->cfg.pools[j->idx];
    tb_arb_snap_t *s = &j->batch->snaps[j->idx]; /* слот фиксирован */
    tb_error_t err = TB_ERROR_INIT;

    if (tb_atomic_load_bool(&a->stopping))
        return; /* остановка: очередь воркеров сворачивается мгновенно */
    memset(s, 0, sizeof(*s));
    s->pool_idx = (uint32_t)j->idx;
    s->pool = p->pool;
    s->token = p->token;
    s->fee = p->fee;
    snprintf(s->dex, sizeof(s->dex), "%s", p->dex);
    s->ts_ms = tb_now_ms();
    s->chain = p->chain;
    s->weth = p->weth;
    /* котировки батча — на блоке СВОЕЙ цепи (В6 + мультичейн) */
    uint64_t block = 0;
    for (uint32_t c = 0; c < j->batch->chains_n; c++)
        if (j->batch->chains[c] == p->chain)
            block = j->batch->blocks[c];
    s->block = block;
    const struct tok_info *ti = tok_find(a, &p->token);
    if (ti) {
        snprintf(s->symbol, sizeof(s->symbol), "%s", ti->symbol);
        s->decimals = ti->decimals;
    }
    /* котировки этого воркера — на блоке цепи пула (В6) */
    tb_exec_dex_set_quote_block(block);

    tb_u256_t probe =
        tb_u256_from_u64(a->cfg.probe_wei ? a->cfg.probe_wei
                                          : 100000000000000ull);
    tb_u256_t big = probe;
    uint64_t mult =
        a->cfg.liquidity_probe_mult ? a->cfg.liquidity_probe_mult : 10;
    (void)tb_u256_mul_u64(&big, &big, mult);

    if (arb_quote_dir(a, &p->pool, p->fee, &p->weth, &p->token, &probe,
                      &s->buy_out, &err) != 0)
        return; /* пустой снапшот отсеется фильтром актуальности */
    if (tb_atomic_load_bool(&a->stopping))
        return;
    if (arb_quote_dir(a, &p->pool, p->fee, &p->weth, &p->token, &big,
                      &s->big_out, &err) != 0)
        return;
    if (arb_quote_dir(a, &p->pool, p->fee, &p->token, &p->weth,
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
    pool_job_t *j = arg;
    arb_batch_t *b = j->batch;
    free(j);
    batch_release(b);
}

/* ==========================================================================
 *  Фаза 2+3: фильтры + Decision Engine (воркер)
 * ========================================================================== */

/* ---- Кеш хоп-котировок решающей фазы (в рамках ОДНОГО батча) ----
 * Все перекотировки пинятся блоком цепи (В6): одинаковый (пул,
 * направление, размер, цепь) в пределах батча даёт одинаковый ответ —
 * ходим в RPC один раз. Пары одного токена делят размеры проб (lo/hi/
 * удвоения), поэтому попадания частые. Линейный скан: ~1024 записей
 * против RPC в сотни мс — копейки. */
#define TB_ARB_HOP_CACHE 1024

typedef struct hop_ent {
    tb_addr_t pool;
    tb_addr_t tin, tout;
    tb_u256_t amt;
    uint64_t  chain;
    tb_u256_t out;
    bool      used;
} hop_ent_t;

typedef struct hop_cache {
    hop_ent_t e[TB_ARB_HOP_CACHE];
    size_t    n;
    bool      full;
} hop_cache_t;

static const tb_u256_t *hop_cache_get(const hop_cache_t *c,
                                      const tb_addr_t *pool,
                                      const tb_addr_t *tin,
                                      const tb_addr_t *tout,
                                      const tb_u256_t *amt,
                                      uint64_t chain)
{
    for (size_t i = 0; i < c->n; i++)
        if (c->e[i].used && c->e[i].chain == chain
            && tb_addr_eq(&c->e[i].pool, pool)
            && tb_addr_eq(&c->e[i].tin, tin)
            && tb_addr_eq(&c->e[i].tout, tout)
            && tb_u256_cmp(&c->e[i].amt, amt) == 0)
            return &c->e[i].out;
    return NULL;
}

static void hop_cache_put(hop_cache_t *c, const tb_addr_t *pool,
                          const tb_addr_t *tin, const tb_addr_t *tout,
                          const tb_u256_t *amt, uint64_t chain,
                          const tb_u256_t *out)
{
    if (c->full)
        return;
    hop_ent_t *e = &c->e[c->n++];
    e->pool = *pool;
    e->tin = *tin;
    e->tout = *tout;
    e->amt = *amt;
    e->chain = chain;
    e->out = *out;
    e->used = true;
    if (c->n == TB_ARB_HOP_CACHE)
        c->full = true;
}



typedef struct decide_job {
    tb_arb_t         *a;
    arb_batch_t      *batch;
    tb_arb_verdict_t  verdicts[TB_ARB_MAX_POOLS];
    size_t            results_n;
    struct res_ent {
        tb_arb_pair_res_t r;
        size_t buy_i, sell_i;
    } results[TB_ARB_MAX_RESULTS];
    tb_arb_route_quote_t rq; /* В7: лучшая пара батча, реальная котировка */
    hop_cache_t       hops;  /* кеш хоп-котировок этого батча */
    /* статистика батча — приватна джобе (воркер), переносится в
     * tb_arb.stats в decide_done на потоке цикла: батчи перекрываются,
     * прямые инкременты из воркеров гонялись (TSan) */
    struct {
        uint64_t filter_rejects, quote_errors;
        uint64_t pairs_evaluated, pairs_profitable;
    } st;
} decide_job_t;

/* Котировщик для tb_arb_eval_pair: пул ищется в watchlist по адресу,
 * ответ кешируется в рамках батча (все перекотировки пинятся блоком
 * цепи — одинаковый запрос даёт одинаковый ответ). */
static int decide_quote(void *ctx, const tb_addr_t *pool, uint32_t fee,
                        const tb_addr_t *tin, const tb_addr_t *tout,
                        const tb_u256_t *amount, tb_u256_t *out,
                        tb_error_t *err)
{
    decide_job_t *d = ctx;
    tb_arb_t *a = d->a;
    /* остановка: рвём на границе ЛЮБОГО хопа — решающая фаза гоняет
     * десятки перекотировок, между парами выхода мало */
    if (tb_atomic_load_bool(&a->stopping)) {
        TB_ERROR_SET(err, TB_DOMAIN_USER, TB_EXEC_BACKEND,
                     "остановка: хоп не котируется");
        return -1;
    }
    for (uint32_t i = 0; i < a->cfg.pools_n; i++)
        if (tb_addr_eq(&a->cfg.pools[i].pool, pool)) {
            uint64_t chain = a->cfg.pools[i].chain;
            const tb_u256_t *hit =
                hop_cache_get(&d->hops, pool, tin, tout, amount, chain);
            if (hit) {
                *out = *hit;
                return 0;
            }
            if (arb_quote_dir(a, pool, fee, tin, tout, amount, out,
                              err) != 0)
                return -1;
            hop_cache_put(&d->hops, pool, tin, tout, amount, chain, out);
            return 0;
        }
    TB_ERROR_SET(err, TB_DOMAIN_USER, TB_EXEC_BACKEND,
                 "decide: пул не из watchlist");
    return -1;
}


static void decide_work(void *arg)
{
    decide_job_t *d = arg;
    tb_arb_t *a = d->a;
    if (tb_atomic_load_bool(&a->stopping))
        return; /* остановка: батч не оцениваем, память почистит dtor */
    uint64_t now = tb_now_ms();

    /* подсказка блока ставится на цепь каждой пары ниже (мультичейн) */

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
            d->st.filter_rejects++;
    }

    for (size_t i = 0; i < d->batch->n && d->results_n < TB_ARB_MAX_RESULTS;
         i++) {
        if (tb_atomic_load_bool(&a->stopping))
            break; /* остановка: решающую фазу не тянем */
        if (!d->verdicts[i].pass)
            continue;
        for (size_t j = 0;
             j < d->batch->n && d->results_n < TB_ARB_MAX_RESULTS; j++) {
            if (i == j || !d->verdicts[j].pass)
                continue;
            if (!tb_addr_eq(&d->batch->snaps[i].token,
                            &d->batch->snaps[j].token))
                continue;
            /* В6/мультичейн: блоки не совпали или цепи разные — курсы
             * несравнимы (номер блока чужой цепи не существует) */
            if (d->batch->snaps[i].chain != d->batch->snaps[j].chain)
                continue;
            if (d->batch->snaps[i].block != d->batch->snaps[j].block)
                continue;
            /* блок неизвестен (0 — eth_blockNumber не донёсся): latest
             * каждого пула — разные состояния цепи, сравнивать нельзя */
            if (d->batch->snaps[i].block == 0)
                continue;
            /* перекотировки решения — на блоке цепи пары; газ цикла —
             * цена газа той же цепи × Units (P1: живой газ, не конфиг).
             * Не достали — 0, движок решения возьмёт gas_est_wei */
            uint64_t gas_cost = 0;
            {
                uint64_t blk = 0;
                for (uint32_t c = 0; c < d->batch->chains_n; c++)
                    if (d->batch->chains[c]
                        == d->batch->snaps[i].chain) {
                        blk = d->batch->blocks[c];
                        if (d->batch->gas_wei[c]) {
                            unsigned __int128 g =
                                (unsigned __int128)d->batch->gas_wei[c]
                                * TB_ARB_GAS_UNITS;
                            gas_cost = g >> 64 ? UINT64_MAX : (uint64_t)g;
                        }
                        break;
                    }
                tb_exec_dex_set_quote_block(blk);
            }

            tb_arb_pair_in_t pin = {
                .buy = &d->batch->snaps[i],
                .sell = &d->batch->snaps[j],
                .gas_cost_wei = gas_cost,
            };
            struct res_ent *e = &d->results[d->results_n];
            tb_error_t qerr = TB_ERROR_INIT;
            int rc = tb_arb_eval_pair(&a->cfg, &pin, decide_quote, d, &e->r,
                                      &qerr);
            if (rc < 0) {
                /* не глотаем: причина и развёрнутые детали — в запись */
                d->st.quote_errors++;
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
            d->st.pairs_evaluated++;
            if (e->r.ok)
                d->st.pairs_profitable++;
        }
    }

    /* В7: лучшая по спреду пара батча — реальная котировка маршрута
     * WETH -> TOKEN -> WETH на probe (оценка спреда может завышать:
     * комиссии обоих хопов и влияние уже внутри числа Quoter). */
    {
        int best = -1;
        uint32_t best_bp = 0;
        for (size_t k = 0; k < d->results_n; k++) {
            struct res_ent *e = &d->results[k];
            if (strncmp(e->r.reason, "ошибка котировщика", 18) == 0)
                continue; /* брак котировщика парой не считаем */
            uint32_t r = 0;
            if (!tb_arb_ratio_bp(&d->batch->snaps[e->buy_i].buy_out,
                                 &d->batch->snaps[e->sell_i].buy_out, &r)
                || r <= 10000)
                continue;
            if (r - 10000 >= best_bp) {
                best_bp = r - 10000;
                best = (int)k;
            }
        }
        if (best >= 0) {
            struct res_ent *e = &d->results[best];
            const tb_arb_snap_t *bs = &d->batch->snaps[e->buy_i];
            const tb_arb_snap_t *ss = &d->batch->snaps[e->sell_i];
            tb_addr_t toks[3] = { bs->weth, bs->token, ss->weth };
            uint32_t fees[2] = { bs->fee, ss->fee };
            tb_error_t rerr = TB_ERROR_INIT;
            tb_route_t r = tb_route_encode(toks, fees, 3, &rerr);
            if (rerr.code == 0) {
                tb_route_set_pool(&r, 0, &bs->pool);
                tb_route_set_pool(&r, 1, &ss->pool);
                /* реальная котировка — на блоке цепи пары */
                for (uint32_t c = 0; c < d->batch->chains_n; c++)
                    if (d->batch->chains[c] == bs->chain)
                        tb_exec_dex_set_quote_block(
                            d->batch->blocks[c]);
                tb_u256_t in = tb_u256_from_u64(
                    a->cfg.probe_wei ? a->cfg.probe_wei
                                     : 100000000000000ull);
                tb_u256_t out;
                if (a->dexb.quote(&a->dexb, &r, &in, tb_now_ms(), &out,
                                  &rerr) == 0) {
                    tb_arb_route_quote_t *q = &d->rq;
                    q->ok = true;
                    snprintf(q->symbol, sizeof(q->symbol), "%s", bs->symbol);
                    snprintf(q->buy_dex, sizeof(q->buy_dex), "%s", bs->dex);
                    snprintf(q->sell_dex, sizeof(q->sell_dex), "%s", ss->dex);
                    q->spread_bp = best_bp;
                    q->in_wei = in;
                    q->out_wei = out;
                    q->profit_neg = tb_u256_cmp(&out, &in) < 0;
                    if (q->profit_neg)
                        (void)tb_u256_sub(&q->profit_wei, &in, &out);
                    else
                        (void)tb_u256_sub(&q->profit_wei, &out, &in);
                }
            }
        }
    }
}

static void decide_done(tb_loop_t *loop, void *arg);

static void decide_dtor(void *arg)
{
    decide_job_t *d = arg;
    batch_release(d->batch); /* слот решающей цепочки (последним — free) */
    free(d);
}

/* ==========================================================================
 *  Журнал: decision.evaluated / decision.submitted / decision.canceled
 * ========================================================================== */

static void emit_evaluated(tb_arb_t *a, arb_batch_t *b, decide_job_t *d)
{
    /* 17+ пулов × ~150 Б/снапшот + пары: 4 КБ уже тесно (усечение
     * ломало JSON-строку журнала) */
    char payload[20 * 1024];
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
        APPEND("%s{\"pool\":\"%s\",\"dex\":\"%s\",\"block\":%llu,"
               "\"age_ms\":%llu,"
               "\"impact_bp\":%u,\"vol_bp\":%u,\"pass\":%s,\"reject\":\"%s\"}",
               i ? "," : "", ph, b->snaps[i].dex,
               (unsigned long long)b->snaps[i].block,
               (unsigned long long)d->verdicts[i].age_ms,
               d->verdicts[i].impact_bp, d->verdicts[i].vol_bp,
               d->verdicts[i].pass ? "true" : "false",
               d->verdicts[i].pass ? "" : d->verdicts[i].reject);
    }
    APPEND("],\"pairs\":[");
    for (size_t k = 0; k < d->results_n; k++) {
        struct res_ent *e = &d->results[k];
        char bp[43], sp[43], sz[80], so[80], os[80], opf[80];
        tb_addr_to_hex(&b->snaps[e->buy_i].pool, bp, sizeof(bp));
        tb_addr_to_hex(&b->snaps[e->sell_i].pool, sp, sizeof(sp));
        tb_u256_to_dec(&e->r.size, sz, sizeof(sz));
        tb_u256_to_dec(&e->r.sell_out, so, sizeof(so));
        tb_u256_to_dec(&e->r.opt_size, os, sizeof(os));
        tb_u256_to_dec(&e->r.opt_profit, opf, sizeof(opf));
        bool same_dex =
            strcmp(b->snaps[e->buy_i].dex, b->snaps[e->sell_i].dex) == 0;
        APPEND("%s{\"buy\":\"%s\",\"sell\":\"%s\",\"ok\":%s,"
               "\"same_dex\":%s,"
               "\"size\":\"%s\",\"sell_out\":\"%s\",\"opt_size\":\"%s\","
               "\"opt_profit_wei\":\"%s\",\"gas_cost_wei\":%llu,"
               "\"probes\":%u,\"reason\":\"%s\"}",
               k ? "," : "", bp, sp, e->r.ok ? "true" : "false",
               same_dex ? "true" : "false", sz, so,
               os, opf, (unsigned long long)e->r.gas_cost_wei,
               e->r.probes, e->r.reason);
    }
    APPEND("]}");
#undef APPEND

    /* переполнение строки журнала — не терять запись: усечь пары
     * (самая тяжёлая часть) и пометить */
    if (strlen(payload) > TB_LEDGER_MAX_LINE - 512) {
        char *p = strstr(payload, ",\"pairs\":[");
        if (p) {
            size_t off = (size_t)(p - payload);
            snprintf(payload + off, sizeof(payload) - off,
                     ",\"pairs_truncated\":true}");
        }
    }
    tb_error_t lerr = TB_ERROR_INIT;
    if (tb_ledger_emit(a->ledger, "decision.evaluated", payload, NULL,
                       NULL, &lerr) != 0)
        /* потеря записи журнала не должна быть молчаливой */
        fprintf(stderr, "arb: журнал: decision.evaluated потерян: %s\n",
                lerr.message);
}

/* ==========================================================================
 *  Фаза 4: подача ордеров (поток цикла)
 * ========================================================================== */

static void decide_done(tb_loop_t *loop, void *arg)
{
    (void)loop;
    decide_job_t *d = arg;
    tb_arb_t *a = d->a;

    /* статистика батча — в общую, на потоке цикла (без атомиков) */
    a->stats.filter_rejects += d->st.filter_rejects;
    a->stats.quote_errors += d->st.quote_errors;
    a->stats.pairs_evaluated += d->st.pairs_evaluated;
    a->stats.pairs_profitable += d->st.pairs_profitable;

    /* веб-страницы: сохранить последние котировки до освобождения батча */
    if (d->batch->seq > a->last_seq) {
        memcpy(a->last_snaps, d->batch->snaps,
               sizeof(tb_arb_snap_t) * d->batch->n);
        { /* публикация: единый момент свежести для всего батча
           * (котировка запинена на один блок) */
            uint64_t pub = tb_now_ms();
            for (size_t i = 0; i < d->batch->n; i++)
                a->last_snaps[i].pub_ms = pub;
        }
        a->last_n = d->batch->n;
        a->last_seq = d->batch->seq;
        a->last_quote = d->rq; /* В7 */
    }

    emit_evaluated(a, d->batch, d);

    uint32_t budget =
        a->cfg.max_pairs_per_poll ? a->cfg.max_pairs_per_poll : 1;

    /* порядок подачи — по максимуму расчётной чистой прибыли (P2):
     * раньше побеждала первая ok-пара по порядку пулов, а не лучшая */
    size_t order[TB_ARB_MAX_RESULTS];
    size_t order_n = 0;
    for (size_t k = 0; k < d->results_n; k++) {
        if (!d->results[k].r.ok)
            continue;
        size_t pos = order_n;
        while (pos > 0 && tb_u256_cmp(&d->results[k].r.opt_profit,
                                      &d->results[order[pos - 1]].r
                                           .opt_profit) > 0) {
            order[pos] = order[pos - 1];
            pos--;
        }
        order[pos] = k;
        order_n++;
    }

    for (size_t oi = 0; oi < order_n && budget > 0; oi++) {
        size_t k = order[oi];
        struct res_ent *e = &d->results[k];
        if (a->pending_n >= TB_ARB_MAX_PENDING)
            break;
        budget--;

        /* ставка — оптимум кривой прибыли (фаза B), не минимум */
        tb_u256_t stake = e->r.opt_size;
        if (tb_u256_is_zero(&stake))
            stake = e->r.size;

        tb_arb_snap_t *bs = &d->batch->snaps[e->buy_i];
        tb_arb_snap_t *ss = &d->batch->snaps[e->sell_i];
        tb_addr_t toks[3] = { bs->weth, bs->token, ss->weth };
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
        o.amount_in = stake;
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
        a->pending[a->pending_n].size = stake;
        a->pending[a->pending_n].min_out = e->r.min_out;
        a->pending[a->pending_n].route = route;
        a->pending_n++;

        char sz[80], mo[80], op[80];
        tb_u256_to_dec(&stake, sz, sizeof(sz));
        tb_u256_to_dec(&e->r.min_out, mo, sizeof(mo));
        tb_u256_to_dec(&e->r.opt_profit, op, sizeof(op));
        char pay[640];
        snprintf(pay, sizeof(pay),
                 "{\"event_id\":%llu,\"size\":\"%s\",\"min_out\":\"%s\","
                 "\"opt_profit_wei\":\"%s\",\"gas_cost_wei\":%llu}",
                 (unsigned long long)ev, sz, mo, op,
                 (unsigned long long)e->r.gas_cost_wei);
        tb_error_t lerr = TB_ERROR_INIT;
        tb_ledger_emit(a->ledger, "decision.submitted", pay, NULL, NULL,
                       &lerr);
    }
}

void batch_maybe_decide(tb_arb_t *a, arb_batch_t *b)
{
    if (b->remaining > 0)
        return; /* не все пулы донесены (зовётся из каждого pool_done) */
    /* remaining дошёл до нуля ровно один раз, но путь отказов submit и
     * финальная проверка submit_pool_jobs могли бы позвать дважды */
    bool expected = false;
    if (!__atomic_compare_exchange_n(&b->decided, &expected, true, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return;
    a->stats.quotes += b->n;

    decide_job_t *d = calloc(1, sizeof(*d));
    if (!d) {
        batch_release(b); /* слот решающей цепочки — наш */
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
        free(d);
        batch_release(b); /* слот решающей цепочки — наш */
    }
    /* успешный submit: слот освободит decide_dtor (последним — free) */
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

/* Пул-джобы батча: слоты фиксированы индексом, remaining ведёт pool_done. */
static void submit_pool_jobs(tb_arb_t *a, arb_batch_t *b, tb_loop_t *loop)
{
    /* счётчик владельцев фиксируем ДО первого submit: быстрая джоба
     * может дойти до dtor раньше, чем мы подадим следующую */
    __atomic_store_n(&b->pool_jobs_n, a->cfg.pools_n, __ATOMIC_RELEASE);
    __atomic_store_n(&b->pools_submitted, true, __ATOMIC_RELEASE);

    if (a->cfg.pools_n == 0) {
        batch_maybe_decide(a, b); /* решающая цепочка заберёт владение */
        return;
    }

    for (uint32_t i = 0; i < a->cfg.pools_n; i++) {
        b->remaining++; /* слот заявлен: ниже либо submit, либо отказ */
        pool_job_t *j = calloc(1, sizeof(*j));
        bool submitted = false;
        if (j) {
            j->a = a;
            j->batch = b;
            j->idx = i;
            tb_pool_submit_t s = {
                .job = pool_work,
                .arg = j,
                .arg_dtor = pool_dtor,
                .loop = loop,
                .done_prio = TB_PRIO_HIGH,
                .done = pool_done,
            };
            submitted = tb_threadpool_submit(tb_loop_pool(loop), &s);
            if (!submitted)
                free(j);
        }
        if (!submitted) {
            b->remaining--;
            if (b->remaining == 0)
                batch_maybe_decide(a, b); /* владение заберёт decide */
            batch_release(b); /* dtor этого слота не придёт */
        }
    }
    /* remaining > 0: решающую цепочку запустит последний pool_done;
     * нулевой remaining при отказах уже обработан внутри цикла */
}

/* Prep-джоба батча (В6 + мультичейн): блок каждой уникальной цепи ДО
 * котировок — весь батч котируется на одном состоянии каждой цепи,
 * пары сравнимы в пределах цепи. Не донеслось — блок 0 (latest). */
typedef struct prep_job {
    tb_arb_t    *a;
    arb_batch_t *batch;
} prep_job_t;

static void prep_work(void *arg)
{
    prep_job_t *j = arg;
    tb_arb_t *a = j->a;

    /* уникальные цепи пула; RPC первой биржи цепи */
    for (uint32_t i = 0; i < a->cfg.pools_n; i++) {
        uint64_t ch = a->cfg.pools[i].chain;
        bool seen = false;
        for (uint32_t c = 0; c < j->batch->chains_n && !seen; c++)
            seen = j->batch->chains[c] == ch;
        if (seen || j->batch->chains_n >= TB_DEX_MAX_VENUES)
            continue;
        const tb_exec_dex_venue_t *v = NULL;
        for (uint32_t k = 0; k < a->cfg.dex.venues_n && !v; k++)
            if (a->cfg.dex.venues[k].chain_id == ch)
                v = &a->cfg.dex.venues[k];
        uint64_t blk = 0;
        if (v) {
            tb_error_t err = TB_ERROR_INIT;
            if (tb_exec_dex_block_number(a->http, v, &blk, &err) != 0)
                blk = 0; /* деградация: latest, блок неизвестен */
        }
        /* газ той же цепи: живая цена на момент батча (P1); не достали —
         * 0, движок решения деградирует на конфиг-фолбэк */
        uint64_t gas = 0;
        if (v) {
            tb_error_t gerr = TB_ERROR_INIT;
            if (tb_exec_dex_gas_price(a->http, v, &gas, &gerr) != 0)
                gas = 0;
        }
        j->batch->chains[j->batch->chains_n] = ch;
        j->batch->blocks[j->batch->chains_n] = blk;
        j->batch->gas_wei[j->batch->chains_n] = gas;
        j->batch->chains_n++;
    }
}

static void prep_done(tb_loop_t *loop, void *arg)
{
    prep_job_t *j = arg;
    submit_pool_jobs(j->a, j->batch, loop);
}

static void prep_dtor(void *arg)
{
    prep_job_t *j = arg;
    arb_batch_t *b = j->batch;
    free(j);
    /* постинг prep-завершения отклонён (остановка): пулы поданы не были
     * (их подаёт prep_done) — единственный владелец батча здесь */
    if (!__atomic_load_n(&b->pools_submitted, __ATOMIC_ACQUIRE))
        free(b);
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

    prep_job_t *pj = calloc(1, sizeof(*pj));
    if (!pj) {
        submit_pool_jobs(a, b, loop); /* без блока: latest */
        return;
    }
    pj->a = a;
    pj->batch = b;

    tb_pool_submit_t s = {
        .job = prep_work,
        .arg = pj,
        .arg_dtor = prep_dtor,
        .loop = loop,
        .done_prio = TB_PRIO_HIGH,
        .done = prep_done,
    };
    if (!tb_threadpool_submit(tb_loop_pool(loop), &s)) {
        free(pj);
        submit_pool_jobs(a, b, loop);
    }
    /* успешный submit: пулы подаст prep_done, владение — у пула */
}

/* ==========================================================================
 *  Фаза 5: монитор отмены невыгодных PENDING-ордеров
 * ========================================================================== */

typedef struct mon_job {
    tb_arb_t  *a;
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
    /* монитор смотрит живой рынок: latest, не блок батча (TLS мог
     * остаться от предыдущей job этого воркера) */
    tb_exec_dex_set_quote_block(0);
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

/* ==========================================================================
 *  Параллельные стартерные проверки: временный пул без цикла (job без
 *  done), ожидание — по счётчику completed пула. 30 пулов + 13 токенов
 *  последовательными eth_call ели ~20 с старта; параллельно — секунды.
 * ========================================================================= */

/* Подождать, пока пул исполнит n заданий (submit уже вернулся). */
static void pool_wait_n(tb_threadpool_t *pool, size_t n)
{
    while (tb_threadpool_completed(pool) < n) {
        struct timespec ts = { .tv_nsec = 500000 };
        nanosleep(&ts, NULL);
    }
}

typedef struct tok_job {
    const tb_arb_cfg_t *cfg;
    tb_http_t         *http;
    struct tok_info    out;  /* addr заполняет постановщик; остальное job */
    uint64_t           chain;
    int                ok;
} tok_job_t;

static void tok_job_work(void *arg)
{
    tok_job_t *j = arg;
    /* RPC первой биржи той же цепи (токен другой цепи на этом RPC
     * не существует) */
    const tb_exec_dex_venue_t *v = &j->cfg->dex.venues[0];
    for (uint32_t k = 0; k < j->cfg->dex.venues_n; k++)
        if (j->cfg->dex.venues[k].chain_id == j->chain) {
            v = &j->cfg->dex.venues[k];
            break;
        }
    tb_error_t terr = TB_ERROR_INIT;
    j->ok = tb_exec_dex_token_info(j->http, v, &j->out.addr,
                                   j->out.symbol, sizeof(j->out.symbol),
                                   &j->out.decimals, &terr) == 0;
    if (!j->ok && terr.retryable) {
        struct timespec ts = { .tv_nsec = 400000000 };
        nanosleep(&ts, NULL);
        tb_error_clear(&terr);
        j->ok = tb_exec_dex_token_info(j->http, v, &j->out.addr,
                                       j->out.symbol,
                                       sizeof(j->out.symbol),
                                       &j->out.decimals, &terr) == 0;
    }
    if (!j->ok) {
        j->out.symbol[0] = '\0';
        j->out.decimals = 0;
    }
}

typedef struct verify_job {
    const tb_arb_cfg_t *cfg;
    tb_http_t         *http;
    uint32_t           pool_idx;
    int                rc;
    tb_error_t         err;
} verify_job_t;

static void verify_job_work(void *arg)
{
    verify_job_t *j = arg;
    const tb_arb_cfg_t *cfg = j->cfg;
    const tb_arb_pool_t *p = &cfg->pools[j->pool_idx];
    const tb_exec_dex_venue_t *v = tb_arb_venue_by_name(cfg, p->dex);
    if (!v) {
        j->rc = -1;
        TB_ERROR_SET(&j->err, TB_DOMAIN_JSON, TB_JSON_SCHEMA,
                     "verify: пул вне venues (сначала validate)");
        return;
    }
    if (tb_addr_is_native(&v->factory))
        return; /* venue без фабрики: не проверяем */
    /* хаб — пула (мультичейн: у Base свой WETH, на RPC Base
     * mainnet-WETH не существует) */
    j->rc = tb_exec_dex_verify_pool(j->http, v, &p->weth, &p->token,
                                    p->fee, &p->pool, &j->err);
    if (j->rc != 0 && j->err.retryable) {
        /* транзиентный сбой RPC не должен валить старт (fail-fast
         * оставляем для настоящих несоответствий) */
        struct timespec ts = { .tv_nsec = 400000000 };
        nanosleep(&ts, NULL);
        tb_error_clear(&j->err);
        j->rc = tb_exec_dex_verify_pool(j->http, v, &p->weth, &p->token,
                                        p->fee, &p->pool, &j->err);
    }
}

int tb_arb_cfg_verify(const tb_arb_cfg_t *cfg, tb_http_t *http,
                      tb_error_t *err)
{
    if (!cfg || !http)
        TB_ERROR_FAIL(err, TB_DOMAIN_CORE, TB_CORE_INVALID_ARG,
                      "verify: аргументы");

    if (cfg->pools_n == 0)
        return 0;

    tb_error_t perr = TB_ERROR_INIT;
    tb_threadpool_t *pool =
        tb_threadpool_create(TB_ARB_START_THREADS, &perr);
    if (!pool) {
        /* без пула — по-старому, последовательно (не смертельно) */
        for (uint32_t i = 0; i < cfg->pools_n; i++) {
            verify_job_t j = {
                .cfg = cfg, .http = http, .pool_idx = i,
                .err = TB_ERROR_INIT,
            };
            verify_job_work(&j);
            if (j.rc != 0) {
                char ph[43];
                tb_addr_to_hex(&cfg->pools[i].pool, ph, sizeof(ph));
                TB_ERROR_SET(err, TB_DOMAIN_USER, TB_EXEC_BACKEND,
                             "пул %s (%s): %s", ph, cfg->pools[i].dex,
                             j.err.message);
                return -1;
            }
        }
        return 0;
    }

    verify_job_t *jobs = calloc(cfg->pools_n, sizeof(*jobs));
    if (!jobs) {
        tb_threadpool_destroy(pool);
        TB_ERROR_FAIL(err, TB_DOMAIN_CORE, TB_CORE_NO_MEMORY, "verify");
    }
    size_t submitted = 0;
    for (uint32_t i = 0; i < cfg->pools_n; i++) {
        jobs[i].cfg = cfg;
        jobs[i].http = http;
        jobs[i].pool_idx = i;
        tb_error_clear(&jobs[i].err); /* calloc обнулил — явно */
        tb_pool_submit_t s = {
            .job = verify_job_work,
            .arg = &jobs[i],
        };
        if (tb_threadpool_submit(pool, &s)) {
            submitted++; /* completed посчитает пул */
        } else {
            verify_job_work(&jobs[i]); /* отказ очереди: здесь же,
                                        * в completed не входит */
        }
    }
    pool_wait_n(pool, submitted);
    tb_threadpool_destroy(pool);

    for (uint32_t i = 0; i < cfg->pools_n; i++) {
        if (jobs[i].rc != 0) {
            char ph[43];
            tb_addr_to_hex(&cfg->pools[i].pool, ph, sizeof(ph));
            TB_ERROR_SET(err, TB_DOMAIN_USER, TB_EXEC_BACKEND,
                         "пул %s (%s): %s", ph, cfg->pools[i].dex,
                         jobs[i].err.message);
            free(jobs);
            return -1;
        }
    }
    free(jobs);
    return 0;
}

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

    /* В5+: сначала лист активов конфига (market asset …) — офлайн-сид
     * symbol/decimals, eth_call только для неизвестных токенов */
    for (uint32_t k = 0; k < a->cfg.assets_n && a->toks_n < TB_ARB_MAX_POOLS;
         k++) {
        const tb_arb_asset_t *as = &a->cfg.assets[k];
        if (!as->symbol[0])
            continue;
        struct tok_info *ti = &a->toks[a->toks_n++];
        memset(ti, 0, sizeof(*ti));
        ti->addr = as->addr;
        snprintf(ti->symbol, sizeof(ti->symbol), "%s", as->symbol);
        ti->decimals = (uint8_t)(as->decimals_known ? as->decimals : 0);
    }

    /* В5: symbol/decimals токенов с цепи, ПАРАЛЛЕЛЬНО (временный пул;
     * последовательные 26 eth_call ели ~10 с старта). Ошибка — не
     * фатальна: пустой символ = короткий адрес на странице. */
    if (a->cfg.dex.venues_n > 0 && a->cfg.pools_n > 0) {
        tok_job_t *tjobs = calloc(a->cfg.pools_n, sizeof(*tjobs));
        if (tjobs) {
            /* уникальные токены (адрес+цепь), хаб не спрашиваем;
             * уже засеянные из листа активов — RPC не тратим */
            size_t unique = 0;
            for (uint32_t i = 0; i < a->cfg.pools_n; i++) {
                const tb_addr_t *t = &a->cfg.pools[i].token;
                if (tb_addr_eq(t, &a->cfg.weth))
                    continue;
                if (tok_find(a, t))
                    continue;
                bool dup = false;
                for (size_t k = 0; k < unique && !dup; k++)
                    dup = tb_addr_eq(&tjobs[k].out.addr, t)
                          && tjobs[k].chain == a->cfg.pools[i].chain;
                if (dup)
                    continue;
                tjobs[unique].cfg = &a->cfg;
                tjobs[unique].http = http;
                tjobs[unique].out.addr = *t;
                tjobs[unique].chain = a->cfg.pools[i].chain;
                unique++;
            }

            tb_error_t perr = TB_ERROR_INIT;
            tb_threadpool_t *pool =
                tb_threadpool_create(TB_ARB_START_THREADS, &perr);
            size_t submitted = 0;
            for (size_t k = 0; k < unique; k++) {
                if (pool) {
                    tb_pool_submit_t s = {
                        .job = tok_job_work,
                        .arg = &tjobs[k],
                    };
                    if (tb_threadpool_submit(pool, &s)) {
                        submitted++;
                        continue;
                    }
                }
                tok_job_work(&tjobs[k]); /* без пула/отказ: здесь же */
            }
            if (pool) {
                pool_wait_n(pool, submitted);
                tb_threadpool_destroy(pool);
            }

            for (size_t k = 0; k < unique && a->toks_n < TB_ARB_MAX_POOLS;
                 k++) {
                a->toks[a->toks_n++] = tjobs[k].out;
                if (!tjobs[k].ok) {
                    char th[43];
                    tb_addr_to_hex(&tjobs[k].out.addr, th, sizeof(th));
                    fprintf(stderr,
                            "arb: токен %s: symbol/decimals не получены, "
                            "покажем адрес\n",
                            th);
                }
            }
            free(tjobs);
        }
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

void tb_arb_request_stop(tb_arb_t *a)
{
    if (a)
        tb_atomic_store_bool(&a->stopping, true);
}

void tb_arb_stop(tb_arb_t *a)
{
    if (!a || !a->running)
        return;
    a->running = false;
    tb_atomic_store_bool(&a->stopping, true);
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

const tb_arb_snap_t *tb_arb_last_snaps(const tb_arb_t *a, size_t *n)
{
    if (n)
        *n = a ? a->last_n : 0;
    return a ? a->last_snaps : NULL;
}

const tb_arb_route_quote_t *tb_arb_route_quote(const tb_arb_t *a)
{
    if (!a || !a->last_quote.ok)
        return NULL;
    return &a->last_quote;
}

uint64_t tb_arb_probe_wei(const tb_arb_t *a)
{
    return a ? a->cfg.probe_wei : 0;
}

/* ==========================================================================
 *  Валидация конфига (В2) и проверка идентичности на цепи (В4)
 * ========================================================================== */

int tb_arb_cfg_validate(const tb_arb_cfg_t *cfg, tb_error_t *err)
{
    if (!cfg)
        TB_ERROR_FAIL(err, TB_DOMAIN_CORE, TB_CORE_INVALID_ARG,
                      "validate: аргументы");
    if (cfg->pools_n == 0)
        TB_ERROR_FAIL(err, TB_DOMAIN_JSON, TB_JSON_SCHEMA,
                      "config: pools пуст — торговать нечем");

    /* В9: реестр строится по pools[].dex в загрузчике, поэтому сверять
     * два объявления больше нечего; здесь — семантика записей и
     * защита ручной сборки cfg. */
    for (uint32_t i = 0; i < cfg->pools_n; i++) {
        const tb_arb_pool_t *p = &cfg->pools[i];
        char ph[43];
        tb_addr_to_hex(&p->pool, ph, sizeof(ph));

        for (uint32_t k = 0; k < i; k++)
            if (tb_addr_eq(&cfg->pools[k].pool, &p->pool))
                TB_ERROR_FAIL(err, TB_DOMAIN_JSON, TB_JSON_SCHEMA,
                              "config: пул %s объявлен дважды "
                              "(pools[%u] и pools[%u])",
                              ph, k, i);
        if (p->dex[0] == '\0')
            TB_ERROR_FAIL(err, TB_DOMAIN_JSON, TB_JSON_SCHEMA,
                          "config: pools[%u].dex пуст", i);
        if (p->fee == 0)
            TB_ERROR_FAIL(err, TB_DOMAIN_JSON, TB_JSON_SCHEMA,
                          "config: pools[%u].fee нулевой (тир V3 обязателен)",
                          i);

        const tb_exec_dex_venue_t *v =
            tb_arb_venue_by_name(cfg, p->dex);
        if (!v)
            TB_ERROR_FAIL(err, TB_DOMAIN_JSON, TB_JSON_SCHEMA,
                          "config: pools[%u].dex '%s' нет в rpc[]", i,
                          p->dex);
        bool listed = false;
        for (uint32_t q = 0; q < v->pools_n && !listed; q++)
            listed = tb_addr_eq(&v->pools[q], &p->pool);
        if (!listed)
            TB_ERROR_FAIL(err, TB_DOMAIN_JSON, TB_JSON_SCHEMA,
                          "config: пул %s (%s) не приписан бирже "
                          "(реестр: pools[].dex)",
                          ph, p->dex);
        /* V2: комиссия пары фиксирована 0.30% — иное значение означало бы
         * чужой пул; V3: тир произвольный */
        if (v->kind == TB_DEX_V2 && p->fee != 3000)
            TB_ERROR_FAIL(err, TB_DOMAIN_JSON, TB_JSON_SCHEMA,
                          "config: пул %s (%s, V2): fee %u, у V2 фиксировано "
                          "3000",
                          ph, p->dex, p->fee);
    }

    /* мультичейн/реестр: имена бирж уникальны (пулы мапятся по имени),
     * хаб-токен один на цепь (курсы и маршруты сравнимы) */
    for (uint32_t v1 = 0; v1 < cfg->dex.venues_n; v1++)
        for (uint32_t v2 = v1 + 1; v2 < cfg->dex.venues_n; v2++) {
            if (strcmp(cfg->dex.venues[v1].name,
                       cfg->dex.venues[v2].name) == 0)
                TB_ERROR_FAIL(err, TB_DOMAIN_JSON, TB_JSON_SCHEMA,
                              "config: две биржи с именем '%s' (реестр "
                              "мапит пулы по имени)",
                              cfg->dex.venues[v1].name);
            if (cfg->dex.venues[v1].chain_id
                    == cfg->dex.venues[v2].chain_id
                && !tb_addr_eq(&cfg->dex.venues[v1].weth,
                               &cfg->dex.venues[v2].weth))
                TB_ERROR_FAIL(err, TB_DOMAIN_JSON, TB_JSON_SCHEMA,
                              "config: цепь %llu: у '%s' и '%s' разные "
                              "хабы (weth)",
                              (unsigned long long)
                                  cfg->dex.venues[v1].chain_id,
                              cfg->dex.venues[v1].name,
                              cfg->dex.venues[v2].name);
        }

    /* один адрес — одна биржа (сводный дедуп; при загрузке из JSON
     * недостижимо, защищает ручную сборку cfg) */
    for (uint32_t v1 = 0; v1 < cfg->dex.venues_n; v1++)
        for (uint32_t q1 = 0; q1 < cfg->dex.venues[v1].pools_n; q1++)
            for (uint32_t v2 = v1; v2 < cfg->dex.venues_n; v2++)
                for (uint32_t q2 = (v2 == v1 ? q1 + 1 : 0);
                     q2 < cfg->dex.venues[v2].pools_n; q2++)
                    if (tb_addr_eq(&cfg->dex.venues[v1].pools[q1],
                                   &cfg->dex.venues[v2].pools[q2]))
                        TB_ERROR_FAIL(err, TB_DOMAIN_JSON, TB_JSON_SCHEMA,
                                      "config: пул приписан двум биржам "
                                      "(%s и %s)",
                                      cfg->dex.venues[v1].name,
                                      cfg->dex.venues[v2].name);
    return 0;
}


