/* ==========================================================================
 *  tests/test_ctx.c — needs-маски шагов: контракт контекста в прогоне.
 *
 *  Продюсер даёт ключи (tb_event_ctx_give), потребитель объявляет
 *  требования (tb_evreg_add_needs): прогон шага без нужных ключей —
 *  FAIL с TB_EV_CONTRACT и точным именованием. Реальная цепочка
 *  исполнения: quote требует ORDER|ROUTE, preflight их даёт.
 * ==========================================================================
 */
#define TB_CURRENT_GROUP TB_G_DEMO

#include <stdio.h>
#include <string.h>

#include "harness.h"
#include "tb/core/error.h"
#include "tb/events/events.h"
#include "tb/exec/engine.h"

#define KIND_TEST 0x5458u /* 'T''X' */

static bool consumer_ran;

static tb_ev_action_t producer_fn(tb_evreg_t *r, tb_event_t *ev, void *u)
{
    (void)r; (void)u;
    tb_event_ctx_give(ev, TB_CTX_MKT_SNAP | TB_CTX_MKT_HIST);
    tb_event_step(r, ev, "продюсер: снапшот и история приложены");
    return TB_EV_ACT_CHAIN;
}

static tb_ev_action_t consumer_fn(tb_evreg_t *r, tb_event_t *ev, void *u)
{
    (void)r; (void)ev; (void)u;
    consumer_ran = true;
    return TB_EV_ACT_DONE;
}

static tb_ev_action_t noop_fn(tb_evreg_t *r, tb_event_t *ev, void *u)
{
    (void)r; (void)ev; (void)u;
    return TB_EV_ACT_CHAIN;
}

int main(void)
{
    tb_error_t err = TB_ERROR_INIT;

    /* ---- синтетика: продюсер дал ключи — потребитель прошёл ---- */
    {
        tb_evreg_t *r = tb_evreg_create(0, &err);
        CHECK(r != NULL);
        CHECK(tb_evreg_add(r, "producer", producer_fn, NULL, &err) >= 0);
        CHECK(tb_evreg_add_needs(r, "consumer", consumer_fn, NULL,
                                 TB_CTX_MKT_SNAP | TB_CTX_MKT_HIST,
                                 &err) >= 0);
        CHECK(tb_evreg_step_needs(r, "consumer")
              == (TB_CTX_MKT_SNAP | TB_CTX_MKT_HIST));
        CHECK(tb_evreg_step_needs(r, "producer") == 0);
        CHECK(tb_evreg_step_needs(r, "нет-такого") == 0);

        tb_event_t *ev = tb_event_create(r, KIND_TEST, 0, &err);
        CHECK(ev != NULL);
        consumer_ran = false;
        CHECK(tb_evreg_dispatch(r, ev, &err) == 0);
        CHECK(ev->state == TB_EV_DONE);
        CHECK(consumer_ran);
        CHECK(ev->ctx_has == (TB_CTX_MKT_SNAP | TB_CTX_MKT_HIST));

        tb_evreg_destroy(r);
    }

    /* ---- без продюсера: контрактное нарушение, шаг не выполнялся ---- */
    {
        tb_evreg_t *r = tb_evreg_create(0, &err);
        CHECK(r != NULL);
        CHECK(tb_evreg_add_needs(r, "consumer", consumer_fn, NULL,
                                 TB_CTX_MKT_SNAP, &err) >= 0);

        tb_event_t *ev = tb_event_create(r, KIND_TEST, 0, &err);
        CHECK(ev != NULL);
        consumer_ran = false;
        CHECK(tb_evreg_dispatch(r, ev, &err) == 0);
        CHECK(ev->state == TB_EV_FAILED);
        CHECK(!consumer_ran); /* шаг не вызывался */
        CHECK(ev->err.domain == TB_DOMAIN_EVENTS);
        CHECK(ev->err.code == TB_EV_CONTRACT);
        CHECK(strstr(ev->err.message, "consumer") != NULL);
        CHECK(strstr(ev->err.message, "не донесён") != NULL);

        tb_evreg_destroy(r);
    }

    /* ---- частичный контекст: не хватает ровно неданного ключа ---- */
    {
        tb_evreg_t *r = tb_evreg_create(0, &err);
        CHECK(r != NULL);
        CHECK(tb_evreg_add(r, "half", noop_fn, NULL, &err) >= 0);
        CHECK(tb_evreg_add_needs(r, "consumer", consumer_fn, NULL,
                                 TB_CTX_MKT_SNAP | TB_CTX_MKT_HIST,
                                 &err) >= 0);
        tb_event_t *ev = tb_event_create(r, KIND_TEST, 0, &err);
        CHECK(ev != NULL);
        /* half не даёт ключей; вручную дадим один из двух */
        tb_event_ctx_give(ev, TB_CTX_MKT_SNAP);
        CHECK(tb_evreg_dispatch(r, ev, &err) == 0);
        CHECK(ev->state == TB_EV_FAILED);
        CHECK(strstr(ev->err.message, "8") != NULL); /* маска недостающего */

        tb_evreg_destroy(r);
    }

    /* ---- реальный конвейер исполнения: quote требует, preflight даёт ---- */
    {
        tb_addr_t weth;
        tb_addr_from_hex(&weth, "0xC02aaA39b223FE8D0A0e5C4F27eAD9083C756Cc2",
                         &err);
        tb_exec_cfg_t cfg = {0};
        cfg.wrapped_native = weth;
        tb_exec_engine_t *e = tb_exec_create(&cfg, &err);
        CHECK(e != NULL);

        CHECK(tb_evreg_step_needs(tb_exec_registry(e), "quote")
              == (TB_CTX_EXEC_ORDER | TB_CTX_EXEC_ROUTE));
        CHECK(tb_evreg_step_needs(tb_exec_registry(e), "preflight") == 0);

        tb_exec_destroy(e);
    }

    printf("OK test_ctx\n");
    return 0;
}
