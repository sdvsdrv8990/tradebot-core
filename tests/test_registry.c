/* tests/test_registry.c — реестр кодов и шаблонов источников. */
#include "harness.h"
#include "tb/core/error_registry.h"

int main(void)
{
    /* ---- метаданные ---- */
    const tb_error_meta_t *m = tb_registry_meta(TB_DOMAIN_API, TB_API_RATE_LIMIT);
    CHECK(m != NULL);
    CHECK_STR_EQ(m->name, "TB_API_RATE_LIMIT");
    CHECK(m->retryable == true);
    CHECK(m->default_severity == TB_SEV_WARN);

    m = tb_registry_meta(TB_DOMAIN_API, TB_API_UNAUTHORIZED);
    CHECK(m != NULL && m->retryable == false);

    CHECK(tb_registry_meta(TB_DOMAIN_API, 9999) == NULL);
    CHECK(tb_registry_meta_count() > 20);

    uint8_t dom;
    uint32_t code;
    CHECK(tb_registry_code_by_name("TB_API_SERVER", &dom, &code));
    CHECK(dom == TB_DOMAIN_API && code == TB_API_SERVER);
    CHECK(!tb_registry_code_by_name("TB_NOPE", &dom, &code));

    /* ---- отображение ответов ---- */
    tb_error_t err = TB_ERROR_INIT;
    tb_error_clear(&err);

    /* успех */
    CHECK(tb_registry_map_response("geckoterminal", 200, "{}", 2, -1, &err) == false);
    CHECK(!tb_error_is_set(&err));

    /* 429 c Retry-After: правило источника */
    CHECK(tb_registry_map_response("geckoterminal", 429, "rate limit", 10, 3000, &err));
    CHECK(err.code == TB_API_RATE_LIMIT);
    CHECK(err.domain == TB_DOMAIN_API);
    CHECK(err.retryable == true);
    CHECK(err.http_status == 429);
    CHECK(err.retry_after_ms == 3000);
    CHECK(strstr(err.message, "geckoterminal") != NULL);

    /* паттерн в теле (coingecko) */
    CHECK(tb_registry_map_response("coingecko", 429, "{\"msg\":\"Rate Limit Exceeded\"}", 30, 1000, &err));
    CHECK(err.code == TB_API_RATE_LIMIT && err.retry_after_ms == 1000);

    /* неизвестный источник, 5xx -> общее правило */
    CHECK(tb_registry_map_response("somewhere", 503, "", 0, -1, &err));
    CHECK(err.code == TB_API_SERVER);
    CHECK(err.retryable == true);

    /* 401 -> перманентная */
    CHECK(tb_registry_map_response("somewhere", 401, "", 0, -1, &err));
    CHECK(err.code == TB_API_UNAUTHORIZED);
    CHECK(err.retryable == false);

    /* 202 -> PENDING */
    CHECK(tb_registry_map_response("coingecko", 202, "", 0, -1, &err));
    CHECK(err.code == TB_API_PENDING);
    CHECK(err.retryable == true);

    /* 404 без правила источника -> общее */
    CHECK(tb_registry_map_response("geckoterminal", 404, NULL, 0, -1, &err));
    CHECK(err.code == TB_API_NOT_FOUND);

    /* эвристика по нестандартному коду 418 */
    CHECK(tb_registry_map_response("x", 418, NULL, 0, -1, &err));
    CHECK(err.code == TB_API_BAD_REQUEST);

    /* ---- пользовательское правило ---- */
    tb_provider_map_t rule = {
        .source = "newsource", .http_status = 418, .pattern = NULL,
        .domain = TB_DOMAIN_API, .code = TB_API_UNSUPPORTED,
        .retryable = false, .note = "teapot",
    };
    tb_error_t rerr = TB_ERROR_INIT;
    CHECK(tb_registry_add_source_map(&rule, &rerr));
    CHECK(tb_registry_map_response("newsource", 418, NULL, 0, -1, &err));
    CHECK(err.code == TB_API_UNSUPPORTED);
    CHECK(err.retryable == false);

    size_t maps_before = tb_registry_map_count();
    tb_provider_map_t dup_rule = rule;
    CHECK(tb_registry_add_source_map(&dup_rule, &rerr)); /* лимит не исчерпан */
    CHECK(tb_registry_map_count() == maps_before + 1);

    printf("OK test_registry (%zu кодов, %zu правил)\n",
           tb_registry_meta_count(), tb_registry_map_count());
    return 0;
}
