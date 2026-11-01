/* tests/test_error.c — ядро обработки ошибок. */
#include "harness.h"
#include "tb/core/error.h"
#include "tb/core/error_registry.h"

static int g_handler_calls;
static tb_severity_t g_last_sev;

static void counting_handler(const tb_error_t *err, void *user)
{
    (void)user;
    g_handler_calls++;
    g_last_sev = (tb_severity_t)err->severity;
}

int main(void)
{
    /* ---- Имена ---- */
    CHECK_STR_EQ(tb_domain_name(TB_DOMAIN_API), "API");
    CHECK_STR_EQ(tb_severity_name(TB_SEV_WARN), "WARN");

    /* ---- POD-безопасность: неинициализированная ошибка не роняет ядро ---- */
    tb_error_t pod;
    memset(&pod, 0xAB, sizeof(pod));
    tb_error_clear(&pod);
    CHECK(!tb_error_is_set(&pod));

    /* ---- set: метаданные из реестра ---- */
    tb_error_t err = TB_ERROR_INIT;
    tb_error_clear(&err);
    CHECK(!tb_error_is_set(&err));

    tb_error_set(&err, TB_DOMAIN_API, TB_API_RATE_LIMIT, "test.c:1",
                 "источник отклонил запрос");
    CHECK(tb_error_is_set(&err));
    CHECK(err.domain == TB_DOMAIN_API);
    CHECK(err.code == TB_API_RATE_LIMIT);
    CHECK(err.severity == TB_SEV_WARN);      /* из реестра */
    CHECK(err.retryable == true);            /* из реестра */
    CHECK_STR_EQ(err.location, "test.c:1");

    /* ---- wrap: контекст добавляется, код сохраняется ---- */
    tb_error_wrap(&err, "test.c:2", "provider=geckoterminal op=get_pool_ohlcv");
    CHECK(err.code == TB_API_RATE_LIMIT);
    CHECK(err.domain == TB_DOMAIN_API);
    CHECK(strstr(err.message, "provider=geckoterminal") != NULL);
    CHECK(strstr(err.message, "источник отклонил запрос") != NULL);
    CHECK(strstr(err.location, "test.c:2") != NULL);
    CHECK(strstr(err.location, "test.c:1") != NULL);

    /* ---- http_status / retry_after ---- */
    err.http_status = 429;
    err.retry_after_ms = 3000;
    char line[512];
    tb_error_format(&err, line, sizeof(line));
    CHECK(strstr(line, "[API/TB_API_RATE_LIMIT") != NULL);
    CHECK(strstr(line, "HTTP 429") != NULL);
    CHECK(strstr(line, "retry_after=3000ms") != NULL);
    CHECK(strstr(line, "retryable") != NULL);

    /* ---- promote: прежнее сообщение сохраняется следом ---- */
    tb_error_promote(&err, TB_DOMAIN_LEDGER, TB_LEDGER_IO, "test.c:3",
                     "журнал недоступен");
    CHECK(err.domain == TB_DOMAIN_LEDGER);
    CHECK(err.code == TB_LEDGER_IO);
    CHECK(strstr(err.message, "журнал недоступен") != NULL);
    CHECK(strstr(err.message, "источник отклонил запрос") != NULL);

    /* ---- POD-копирование присваиванием ---- */
    tb_error_t copy = err;
    CHECK(copy.domain == TB_DOMAIN_LEDGER);
    CHECK(copy.code == TB_LEDGER_IO);
    tb_error_clear(&copy);
    CHECK(!tb_error_is_set(&copy));

    /* ---- clear ---- */
    tb_error_clear(&err);
    CHECK(!tb_error_is_set(&err));

    /* ---- глобальный обработчик ---- */
    g_handler_calls = 0;
    tb_set_error_handler(counting_handler, NULL);
    tb_error_set(&err, TB_DOMAIN_API, TB_API_SERVER, "t", "5xx");
    tb_report(&err);
    tb_report(&err);
    CHECK(g_handler_calls == 2);
    CHECK(g_last_sev == TB_SEV_WARN);
    tb_set_error_handler(NULL, NULL);
    tb_report(&err); /* обработчика нет — просто no-op */
    CHECK(g_handler_calls == 2);

    tb_error_clear(&err);
    printf("OK test_error\n");
    return 0;
}
