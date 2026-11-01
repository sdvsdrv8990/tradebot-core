/* tests/test_exec.cpp — интеграционный тест механизма торговли.
 *
 * Прогоняет ордера через полный конвейер (preflight -> ... -> commit):
 * заполнение, PENDING с поллингом, retryable-отказ с повтором, slippage,
 * нехватка баланса, дедлайн, нулевая сумма, рассинхрон маршрута, отказ
 * ликвидности. Записи trade.* из журнала разбираются строгим парсером
 * конвертов (tb_envelope_parse) — проверка конвенции Universal API Runtime.
 */
#include "harness.h"

#include "tb/arch/arch.h"
#include "tb/core/time.h"
#include "tb/events/events.h"
#include "tb/exec/engine.h"
#include "tb/exec/u256.h"
#include "tb/exec/wallet.h"
#include "tb/json/envelope.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

/* ---- адреса стенда ---- */
static tb_addr_t A(const char *hex)
{
    tb_addr_t a;
    CHECK(tb_addr_from_hex(&a, hex, nullptr) == 0);
    return a;
}
static tb_addr_t WETH = A("0xeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee");
static tb_addr_t TOK_A = A("0xaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
static tb_addr_t TOK_B = A("0xbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");

static tb_u256_t dec(const char *s)
{
    tb_u256_t v;
    CHECK(tb_u256_from_dec(&v, s, strlen(s), nullptr) == 0);
    return v;
}
static std::string str(const tb_u256_t &v)
{
    char buf[TB_U256_DEC_MAX];
    tb_u256_to_dec(&v, buf, sizeof(buf));
    return std::string(buf);
}

static const char *LEDGER_PATH = "test_exec_ledger.jsonl";

/* Ожидаемая котировка симулятора, вычисленная независимо от движка. */
static tb_u256_t expected_quote_single(uint64_t seed, const tb_route_t &route,
                                       const tb_u256_t &amount_in)
{
    auto price_fp6 = [&](const tb_addr_t &tok) -> uint32_t {
        uint8_t buf[20 + 8];
        memcpy(buf, tok.b, 20);
        memcpy(buf + 20, &seed, 8);
        return 500000u + tb_crc32c(0, buf, sizeof(buf)) % 500000u;
    };
    tb_u256_t v = amount_in;
    size_t hops = tb_route_hops(&route);
    for (size_t hop = 0; hop < hops; hop++) {
        tb_addr_t a = tb_route_token(&route, hop);
        tb_addr_t b = tb_route_token(&route, hop + 1);
        uint32_t fee = tb_route_fee(&route, hop);
        CHECK(tb_u256_mul_u64(&v, &v, price_fp6(a)));
        tb_u256_div_u64(&v, &v, price_fp6(b));
        CHECK(tb_u256_mul_u64(&v, &v, 1000000u - fee));
        tb_u256_div_u64(&v, &v, 1000000u);
    }
    uint32_t impact_div = 2000u + tb_crc32c(0, route.bytes, route.len) % 3000u;
    tb_u256_t impact;
    tb_u256_div_u64(&impact, &v, impact_div);
    CHECK(tb_u256_sub(&v, &v, &impact));
    return v;
}

static tb_u256_t scale(const tb_u256_t &v, uint64_t num, uint64_t den)
{
    tb_u256_t r = v;
    CHECK(tb_u256_mul_u64(&r, &r, num));
    tb_u256_div_u64(&r, &r, den);
    return r;
}

/* Обёртки под стиль теста: сумма строкой, адрес/маршрут по ссылке. */
static bool deposit(tb_wallet_t *w, const tb_addr_t &a, const char *amt,
                    tb_error_t *err)
{
    tb_u256_t v = dec(amt);
    return tb_wallet_deposit(w, &a, &v, err) == 0;
}
static bool quote(tb_exec_engine_t *e, const tb_route_t &r, const char *amt,
                  tb_u256_t *out, tb_error_t *err)
{
    tb_u256_t v = dec(amt);
    return tb_exec_quote(e, &r, &v, out, err) == 0;
}
static std::string bal(tb_wallet_t *w, const tb_addr_t &a)
{
    return str(tb_wallet_balance(w, &a));
}

/* Прогнать ордер до терминала и вернуть исход. Событие с подключённым
 * журналом закрывается синхронно с терминалом, поэтому исход читается
 * из дельт статистики движка — это открытый API, а не внутренности. */
struct Outcome {
    tb_ev_state_t st;
    uint64_t retries_delta;
    uint64_t pending_delta;
};

static Outcome run_order(tb_exec_engine_t *e, const tb_exec_order_t &o,
                         tb_error_t &err)
{
    const tb_exec_stats_t *s0 = tb_exec_stats(e);
    uint64_t f0 = s0->filled, fa0 = s0->failed, c0 = s0->canceled;
    uint64_t r0 = s0->retries, p0 = s0->pending_records;

    Outcome out = {};
    uint64_t id = tb_exec_submit(e, &o, &err);
    if (id == 0)
        return out;

    for (int guard = 0; guard < 200; guard++) {
        tb_event_t *ev = tb_evreg_find(tb_exec_registry(e), id);
        if (!ev)
            break; /* закрыт журналом после терминала */
        if (ev->state == TB_EV_DONE || ev->state == TB_EV_FAILED ||
            ev->state == TB_EV_CANCELED) {
            out.st = (tb_ev_state_t)ev->state;
            break;
        }
        if (ev->state == TB_EV_BLOCKED) {
            uint64_t now = tb_now_ms();
            if (ev->wake_at_ms > now)
                tb_sleep_ms(ev->wake_at_ms - now + 1);
            tb_error_t cerr = TB_ERROR_INIT;
            if (tb_evreg_continue(tb_exec_registry(e), ev, &cerr) != 0)
                break;
            continue;
        }
        break; /* QUEUED/ACTIVE вне прогона не бывает */
    }

    const tb_exec_stats_t *s1 = tb_exec_stats(e);
    out.retries_delta = s1->retries - r0;
    out.pending_delta = s1->pending_records - p0;
    if (out.st != TB_EV_DONE && out.st != TB_EV_FAILED &&
        out.st != TB_EV_CANCELED) {
        if (s1->filled > f0)        out.st = TB_EV_DONE;
        else if (s1->failed > fa0)  out.st = TB_EV_FAILED;
        else if (s1->canceled > c0) out.st = TB_EV_CANCELED;
    }
    return out;
}

/* ---- инъекция бэкенда (v0.3): минимальный заимствованный адаптер ---- */
static struct {
    int  calls_quote, calls_submit, calls_poll;
    bool destroyed;
} inj_state;

static void inj_destroy_backend(tb_exec_backend_t *b)
{
    inj_state.destroyed = true;
    (void)b;
}

static int inj_quote(tb_exec_backend_t *b, const tb_route_t *route,
                     const tb_u256_t *amount_in, uint64_t now_ms,
                     tb_u256_t *out, tb_error_t *err)
{
    (void)b; (void)route; (void)now_ms; (void)err;
    inj_state.calls_quote++;
    /* котировка: вход * 2 (без переполнений на масштабах теста) */
    return tb_u256_mul_u64(out, amount_in, 2) ? 0 : -1;
}

static int inj_submit(tb_exec_backend_t *b, const tb_route_t *route,
                      const tb_u256_t *amount_in, const tb_u256_t *min_out,
                      uint64_t deadline_ms, uint64_t now_ms,
                      uint64_t *tx_id, tb_u256_t *amount_out, tb_error_t *err)
{
    (void)b; (void)route; (void)min_out; (void)deadline_ms; (void)now_ms;
    inj_state.calls_submit++;
    *tx_id = 0;
    return inj_quote(b, route, amount_in, now_ms, amount_out, err);
}

static int inj_poll(tb_exec_backend_t *b, uint64_t tx_id, uint64_t now_ms,
                    tb_u256_t *amount_out, tb_error_t *err)
{
    (void)b; (void)tx_id; (void)now_ms; (void)amount_out; (void)err;
    inj_state.calls_poll++;
    return -1;
}

static tb_exec_backend_t inj_backend = {
    .name    = "inj-test",
    .destroy = inj_destroy_backend,
    .quote   = inj_quote,
    .submit  = inj_submit,
    .poll    = inj_poll,
};

int main(void)
{
    tb_error_t err = TB_ERROR_INIT;
    remove(LEDGER_PATH);
    tb_arch_init();

    /* ---- конфигурация движка ---- */
    tb_exec_sim_scenario_t script[] = {
        { TB_SIM_FILL,   0,   997, 1000, TB_EXEC_OK, false, "немедленное заполнение" },
        { TB_SIM_SUBMIT, 250, 995, 1000, TB_EXEC_OK, false, "принято, заполнится через 250 мс" },
        { TB_SIM_REJECT, 0,   0,   0,    TB_EXEC_BACKEND, true, "503 от DEX-адаптера (демо)" },
        { TB_SIM_FILL,   0,   990, 1000, TB_EXEC_OK, false, "заполнение после повтора" },
        { TB_SIM_FILL,   0,   999, 1000, TB_EXEC_OK, false, "заполнение к slippage-проверке" },
        { TB_SIM_REJECT, 0,   0,   0,    TB_EXEC_INSUFFICIENT_LIQUIDITY, false,
          "маршрут тонкий: ликвидности мало" },
        { TB_SIM_FILL,   0,   993, 1000, TB_EXEC_OK, false, "нативный вход: заполнение" },
        { TB_SIM_FILL,   0,   997, 1000, TB_EXEC_OK, false, "нативный выход: заполнение" },
    };

    tb_exec_cfg_t cfg = {};
    cfg.wrapped_native       = WETH;
    cfg.script               = script;
    cfg.script_n             = 8;
    cfg.seed                 = 42;
    cfg.rl_rate_per_min      = 600;
    cfg.rl_burst             = 100;
    cfg.br_threshold         = 5;
    cfg.br_cooldown_ms       = 1000;
    cfg.backoff_base_ms      = 60;
    cfg.backoff_cap_ms       = 200;
    cfg.backoff_jitter       = 0.0;
    cfg.fill_poll_max        = 6;
    cfg.fill_poll_interval_ms= 70;
    cfg.max_slippage_bp      = 100;
    cfg.min_deadline_ahead_ms= 100;

    tb_exec_engine_t *e = tb_exec_create(&cfg, &err);
    CHECK(e != nullptr);

    /* Депозиты стенда. */
    tb_wallet_t *w = (tb_wallet_t *)tb_exec_wallet(e);
    tb_addr_t native = tb_addr_zero();
    CHECK(deposit(w, TOK_A, "100000000000000000000", &err));
    CHECK(deposit(w, TOK_B, "50000000000000000000", &err));
    CHECK(deposit(w, native, "10000000000000000000", &err));

    /* Журнал (терминалы -> event.trace + закрытие). */
    tb_ledger_t *ledger = tb_ledger_open(LEDGER_PATH, TB_LEDGER_FLUSH_LINE, &err);
    CHECK(ledger != nullptr);
    CHECK(tb_exec_attach_ledger(e, ledger) >= 0);

    uint64_t deadline = tb_wall_ms() + 60000;
    tb_route_t r_ab = tb_route_single(&TOK_A, &TOK_B, 3000, &err);
    tb_route_t r_ba = tb_route_single(&TOK_B, &TOK_A, 3000, &err);
    tb_route_t r_wb = tb_route_single(&WETH, &TOK_B, 500, &err);
    tb_route_t r_bw = tb_route_single(&TOK_B, &WETH, 3000, &err);
    CHECK(tb_route_valid(&r_ab) && tb_route_valid(&r_ba));
    CHECK(tb_route_valid(&r_wb) && tb_route_valid(&r_bw));

    /* ---- котировка: формульная проверка симулятора ---- */
    tb_u256_t q_ab;
    CHECK(quote(e, r_ab, "1000000000000000000", &q_ab, &err));
    CHECK(str(q_ab) == str(expected_quote_single(42, r_ab, dec("1000000000000000000"))));


    /* ---- T1: обычное заполнение A -> B ---- */
    {
        tb_exec_order_t o = {};
        o.token_in = TOK_A; o.token_out = TOK_B;
        o.amount_in = dec("1000000000000000000");
        o.deadline_ms = deadline;
        o.route = r_ab;
        Outcome out = run_order(e, o, err);
        CHECK(out.st == TB_EV_DONE);
        CHECK(out.retries_delta == 0);
    }
    tb_u256_t out1 = scale(q_ab, 997, 1000);

    /* ---- T2: PENDING -> поллинг -> заполнение B -> A ---- */
    {
        tb_exec_order_t o = {};
        o.token_in = TOK_B; o.token_out = TOK_A;
        o.amount_in = dec("2000000000000000000");
        o.deadline_ms = deadline;
        o.route = r_ba;
        Outcome out = run_order(e, o, err);
        CHECK(out.st == TB_EV_DONE);
        CHECK(out.pending_delta == 1); /* событие прошло через PENDING */
    }
    tb_u256_t q_ba;
    CHECK(quote(e, r_ba, "2000000000000000000", &q_ba, &err));
    tb_u256_t out2 = scale(q_ba, 995, 1000);

    /* ---- T3: retryable-отказ -> backoff -> повтор -> заполнение ---- */
    {
        tb_exec_order_t o = {};
        o.token_in = TOK_A; o.token_out = TOK_B;
        o.amount_in = dec("1000000000000000000");
        o.deadline_ms = deadline;
        o.route = r_ab;
        Outcome out = run_order(e, o, err);
        CHECK(out.st == TB_EV_DONE);
        CHECK(out.retries_delta == 1); /* первый прогон — отказ, второй — заполнение */
    }
    tb_u256_t out3 = scale(q_ab, 990, 1000);

    /* ---- T4: slippage: минимум выше выхода -> FAILED, балансы целы ---- */
    {
        tb_u256_t before_a = tb_wallet_balance(w, &TOK_A);
        tb_exec_order_t o = {};
        o.token_in = TOK_A; o.token_out = TOK_B;
        o.amount_in = dec("1000000000000000000");
        o.amount_out_min = dec("5000000000000000000"); /* заведомо много */
        o.deadline_ms = deadline;
        o.route = r_ab;
        Outcome out = run_order(e, o, err);
        CHECK(out.st == TB_EV_FAILED);
        CHECK(str(tb_wallet_balance(w, &TOK_A)) == str(before_a));
    }

    /* ---- T5: нехватка баланса ---- */
    {
        tb_exec_order_t o = {};
        o.token_in = TOK_B; o.token_out = TOK_A;
        o.amount_in = dec("999000000000000000000");
        o.deadline_ms = deadline;
        o.route = r_ba;
        Outcome out = run_order(e, o, err);
        CHECK(out.st == TB_EV_FAILED);
    }

    /* ---- T6: дедлайн в прошлом ---- */
    {
        tb_exec_order_t o = {};
        o.token_in = TOK_A; o.token_out = TOK_B;
        o.amount_in = dec("1000000000000000000");
        o.deadline_ms = tb_wall_ms() - 1000;
        o.route = r_ab;
        Outcome out = run_order(e, o, err);
        CHECK(out.st == TB_EV_FAILED);
    }

    /* ---- T7: нулевая сумма ---- */
    {
        tb_exec_order_t o = {};
        o.token_in = TOK_A; o.token_out = TOK_B;
        o.deadline_ms = deadline;
        o.route = r_ab;
        Outcome out = run_order(e, o, err);
        CHECK(out.st == TB_EV_FAILED);
    }

    /* ---- T8: маршрут не согласован с ордером ---- */
    {
        tb_exec_order_t o = {};
        o.token_in = TOK_A; o.token_out = TOK_B;
        o.amount_in = dec("1000000000000000000");
        o.deadline_ms = deadline;
        o.route = r_ba; /* начинается с B, а вход — A */
        Outcome out = run_order(e, o, err);
        CHECK(out.st == TB_EV_FAILED);
    }

    /* ---- T9: перманентный отказ бэкенда (ликвидность) ---- */
    {
        tb_u256_t before_a = tb_wallet_balance(w, &TOK_A);
        tb_exec_order_t o = {};
        o.token_in = TOK_A; o.token_out = TOK_B;
        o.amount_in = dec("1000000000000000000");
        o.deadline_ms = deadline;
        o.route = r_ab;
        Outcome out = run_order(e, o, err);
        CHECK(out.st == TB_EV_FAILED);
        CHECK(str(tb_wallet_balance(w, &TOK_A)) == str(before_a));
    }

    /* ---- T10: нативный вход (wrap -> маршрутизатор, wrapped не остаётся) ---- */
    {
        tb_exec_order_t o = {};
        o.token_in = tb_addr_zero(); o.token_out = TOK_B;
        o.amount_in = dec("2000000000000000000");
        o.deadline_ms = deadline;
        o.route = r_wb;
        Outcome out = run_order(e, o, err);
        CHECK(out.st == TB_EV_DONE);
    }
    tb_u256_t q_wb;
    CHECK(quote(e, r_wb, "2000000000000000000", &q_wb, &err));
    tb_u256_t out10 = scale(q_wb, 993, 1000);

    /* ---- T11: нативный выход (router платит wrapped, разворачиваем) ---- */
    {
        tb_exec_order_t o = {};
        o.token_in = TOK_B; o.token_out = tb_addr_zero();
        o.amount_in = dec("3000000000000000000");
        o.deadline_ms = deadline;
        o.route = r_bw;
        Outcome out = run_order(e, o, err);
        CHECK(out.st == TB_EV_DONE);
    }
    tb_u256_t q_bw;
    CHECK(quote(e, r_bw, "3000000000000000000", &q_bw, &err));
    tb_u256_t out11 = scale(q_bw, 997, 1000);

    /* ---- балансы: атомарность и точная математика ---- */
    /* A: 100e18 - 1e18 (T1) + out2 (T2) - 1e18 (T3) ; T4..T9 не меняли */
    tb_u256_t expect_a = dec("98000000000000000000");
    CHECK(tb_u256_add(&expect_a, &expect_a, &out2));
    CHECK(str(tb_wallet_balance(w, &TOK_A)) == str(expect_a));
    /* B: 50e18 - 2e18 (T2) + out1 (T1) + out3 (T3) + out10 (T10) - 3e18 (T11) */
    tb_u256_t expect_b = dec("45000000000000000000");
    CHECK(tb_u256_add(&expect_b, &expect_b, &out1));
    CHECK(tb_u256_add(&expect_b, &expect_b, &out3));
    CHECK(tb_u256_add(&expect_b, &expect_b, &out10));
    CHECK(str(tb_wallet_balance(w, &TOK_B)) == str(expect_b));
    /* нативный: 10e18 - 2e18 (wrap на входе T10) + out11 (unwrap T11) */
    tb_u256_t expect_n = dec("8000000000000000000");
    CHECK(tb_u256_add(&expect_n, &expect_n, &out11));
    CHECK(str(tb_wallet_balance(w, &native)) == str(expect_n));
    /* wrapped-актив НЕ остаётся: контракт не держит WETH (инвариант) */
    CHECK(bal(w, WETH) == "0");
    /* allowance после всех сделок — ноль (forceApprove -> 0) */
    {
        tb_u256_t a = tb_wallet_allowance(w, &TOK_A);
        CHECK(tb_u256_is_zero(&a));
    }

    /* ---- статистика ---- */
    const tb_exec_stats_t *es = tb_exec_stats(e);
    CHECK(es->submitted == 11);
    CHECK(es->filled == 5);
    CHECK(es->failed == 6);
    CHECK(es->slippage_rejects == 1);
    CHECK(es->backend_rejects == 1);
    CHECK(es->timeouts == 0);
    CHECK(es->retries == 1);
    CHECK(es->pending_records == 1);

    tb_exec_destroy(e);
    tb_ledger_close(ledger);

    /* ---- журнал: строгая проверка записей конвертов ---- */
    FILE *f = fopen(LEDGER_PATH, "r");
    CHECK(f != nullptr);
    int executed = 0, submitted = 0, failed = 0, traces = 0, other = 0;
    char line[TB_LEDGER_MAX_LINE + 8];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        if (len == 0)
            continue;

        const char *type = strstr(line, "\"type\":\"");
        CHECK(type != nullptr);
        if (strstr(type, "event.trace")) {
            traces++;
            continue;
        }
        bool is_exec = strstr(type, "\"type\":\"trade.executed\"") != nullptr;
        bool is_subm = strstr(type, "\"type\":\"trade.submitted\"") != nullptr;
        bool is_fail = strstr(type, "\"type\":\"trade.failed\"") != nullptr;
        if (!is_exec && !is_subm && !is_fail) {
            other++;
            continue;
        }

        /* payload — последнее поле строки: от '{' после "payload": до
         * закрывающей скобки САМОГО payload (строка заканчивается
         * "payload":{...}} — внешняя '}' журнала не входит). */
        const char *pl = strstr(line, "\"payload\":");
        CHECK(pl != nullptr);
        const char *start = strchr(pl + 10, '{');
        CHECK(start != nullptr);
        const char *end = line + len - 2; /* без внешней '}' строки журнала */
        CHECK(end > start && *end == '}');

        std::string payload(start, (size_t)(end - start + 1));
        tb_envelope_t env;
        if (tb_envelope_parse(payload.c_str(), payload.size(), &env, &err) != 0) {
            fprintf(stderr, "FAIL parse: type=%s err=%s\npayload=%s\n",
                    is_exec ? "executed" : is_subm ? "submitted" : "failed",
                    err.message, payload.c_str());
            exit(1);
        }

        if (is_exec) {
            executed++;
            CHECK(env.status == TB_ENV_SUCCESS);
            CHECK(strcmp(env.operation, "swap_exact_in") == 0);
            CHECK(strcmp(env.source, "sim-dex") == 0);
            CHECK(env.data_json[0] == '{');
        } else if (is_subm) {
            submitted++;
            CHECK(env.status == TB_ENV_PENDING);
            CHECK(env.next_attempt_at > 0);
        } else {
            failed++;
            CHECK(env.status == TB_ENV_ERROR);
            CHECK(env.error.code != 0);
        }
    }
    fclose(f);
    remove(LEDGER_PATH);

    CHECK(executed == 5);
    CHECK(submitted == 1);
    CHECK(failed == 6);
    CHECK(traces == 11); /* каждый терминал — event.trace */
    CHECK(other == 0);

    /* ---- инъекция бэкенда (v0.3): заимствованный адаптер вместо
     * симулятора; движок пользуется, но НЕ разрушает (владение у нас) ---- */
    {
        tb_exec_cfg_t inj_cfg = {};
        inj_cfg.wrapped_native = WETH;
        inj_cfg.backend        = &inj_backend; /* инъекция вместо симулятора */

        tb_exec_engine_t *e2 = tb_exec_create(&inj_cfg, &err);
        CHECK(e2 != nullptr);

        tb_wallet_t *w2 = (tb_wallet_t *)tb_exec_wallet(e2);
        CHECK(deposit(w2, TOK_A, "1000000000000000000", &err));

        tb_exec_order_t o = {};
        o.token_in   = TOK_A;
        o.token_out  = TOK_B;
        o.amount_in  = dec("1000000000000000");
        o.deadline_ms = tb_wall_ms() + 5000; /* дедлайн — wall-время */
        o.route      = tb_route_single(&TOK_A, &TOK_B, 3000, &err);

        Outcome out = run_order(e2, o, err);
        CHECK(out.st == TB_EV_DONE);
        CHECK(inj_state.calls_quote >= 1);
        CHECK(inj_state.calls_submit == 1);
        CHECK(inj_state.destroyed == false);

        tb_exec_destroy(e2);
        CHECK(inj_state.destroyed == false); /* заимствован — не тронут */
        inj_destroy_backend(&inj_backend);   /* владелец разрушает сам */
        CHECK(inj_state.destroyed == true);
    }

    printf("OK test_exec (executed=%d submitted=%d failed=%d traces=%d)\n",
           executed, submitted, failed, traces);
    return 0;
}
