/* tests/test_envelope.cpp — API Response Envelope: сериализация и разбор. */
#include "harness.h"
#include "tb/json/envelope.h"
#include "tb/core/error_registry.h"

int main(void)
{
    tb_error_t err = TB_ERROR_INIT;

    /* ---- SUCCESS ---- */
    tb_envelope_t env;
    tb_envelope_init(&env, "geckoterminal", "get_pool_ohlcv", 142);
    CHECK(env.status == TB_ENV_SUCCESS);
    CHECK(tb_envelope_set_data(&env,
          "{\"pool\":\"0x1a2b\",\"price\":1.25,\"volume_24h\":125000}", &err) == 0);

    char buf[2048];
    size_t len = 0;
    CHECK(tb_envelope_serialize(&env, buf, sizeof(buf), &len, &err) == 0);
    CHECK(len > 0);
    CHECK(strstr(buf, "\"schema_version\":\"1.0\"") != NULL);
    CHECK(strstr(buf, "\"source\":\"geckoterminal\"") != NULL);
    CHECK(strstr(buf, "\"operation\":\"get_pool_ohlcv\"") != NULL);
    CHECK(strstr(buf, "\"status\":\"SUCCESS\"") != NULL);
    CHECK(strstr(buf, "\"request_id\":142") != NULL);
    CHECK(strstr(buf, "\"price\":1.25") != NULL);
    CHECK(strstr(buf, "\"error\":null") != NULL);
    CHECK(strstr(buf, "\"retry\":null") != NULL);

    /* Roundtrip. */
    tb_envelope_t back;
    CHECK(tb_envelope_parse(buf, len, &back, &err) == 0);
    CHECK(back.status == TB_ENV_SUCCESS);
    CHECK_STR_EQ(back.source, "geckoterminal");
    CHECK_STR_EQ(back.operation, "get_pool_ohlcv");
    CHECK(back.request_id == 142);
    CHECK(back.timestamp == env.timestamp);
    CHECK_STR_EQ(back.data_json, env.data_json);

    /* ---- ERROR (429 + Retry-After) ---- */
    tb_error_set(&err, TB_DOMAIN_API, TB_API_RATE_LIMIT, "test", "too many requests");
    err.http_status = 429;
    err.retry_after_ms = 3000;
    tb_envelope_init(&env, "coingecko", "historical_analysis", 143);
    CHECK(tb_envelope_set_error(&env, &err) == 0);
    tb_envelope_set_retry(&env, 2, 1793470040123ll);

    CHECK(tb_envelope_serialize(&env, buf, sizeof(buf), &len, &err) == 0);
    CHECK(strstr(buf, "\"status\":\"ERROR\"") != NULL);
    CHECK(strstr(buf, "\"code\":\"TB_API_RATE_LIMIT\"") != NULL);
    CHECK(strstr(buf, "\"domain\":\"API\"") != NULL);
    CHECK(strstr(buf, "\"severity\":\"WARN\"") != NULL);
    CHECK(strstr(buf, "\"retryable\":true") != NULL);
    CHECK(strstr(buf, "\"http_status\":429") != NULL);
    CHECK(strstr(buf, "\"retry_after_ms\":3000") != NULL);
    CHECK(strstr(buf, "\"attempt\":2") != NULL);
    CHECK(strstr(buf, "\"next_attempt_at\":1793470040123") != NULL);

    CHECK(tb_envelope_parse(buf, len, &back, &err) == 0);
    CHECK(back.status == TB_ENV_ERROR);
    CHECK(back.error.domain == TB_DOMAIN_API);
    CHECK(back.error.code == TB_API_RATE_LIMIT);
    CHECK(back.error.retryable == true);
    CHECK(back.error.http_status == 429);
    CHECK(back.error.retry_after_ms == 3000);
    CHECK(back.attempt == 2);
    CHECK(back.next_attempt_at == 1793470040123ll);

    /* ---- PENDING (202): без объекта error, только retry ---- */
    tb_envelope_init(&env, "coingecko", "historical_analysis", 144);
    tb_envelope_set_retry(&env, 3, -1);
    env.status = TB_ENV_PENDING; /* код PENDING — незавершённая задача, не сбой */
    CHECK(tb_envelope_serialize(&env, buf, sizeof(buf), &len, &err) == 0);
    CHECK(strstr(buf, "\"status\":\"PENDING\"") != NULL);
    CHECK(strstr(buf, "\"next_attempt_at\":null") != NULL);
    CHECK(strstr(buf, "\"error\":null") != NULL);
    CHECK(tb_envelope_parse(buf, len, &back, &err) == 0);
    CHECK(back.status == TB_ENV_PENDING);
    CHECK(!tb_error_is_set(&back.error));
    CHECK(back.attempt == 3);
    CHECK(back.next_attempt_at == -1);

    /* ---- Малый буфер ---- */
    char tiny[16];
    CHECK(tb_envelope_serialize(&env, tiny, sizeof(tiny), &len, &err) == -1);
    CHECK(err.domain == TB_DOMAIN_JSON && err.code == TB_JSON_BUFFER);

    /* ---- Некорректный вход ---- */
    CHECK(tb_envelope_parse("{broken", 8, &back, &err) == -1);
    CHECK(err.code == TB_JSON_PARSE);

    CHECK(tb_envelope_parse("{\"source\":\"x\",\"operation\":\"y\",\"status\":\"WEIRD\"}",
                            51, &back, &err) == -1);
    CHECK(err.code == TB_JSON_SCHEMA);

    /* Неизвестный код ошибки в envelope — отказ (стандартизация строга). */
    const char *unknown =
        "{\"source\":\"x\",\"operation\":\"y\",\"status\":\"ERROR\","
        "\"error\":{\"code\":\"TB_NOT_A_CODE\"}}";
    CHECK(tb_envelope_parse(unknown, strlen(unknown), &back, &err) == -1);
    CHECK(err.code == TB_JSON_SCHEMA);

    /* data не-объект — отказ схемы. */
    const char *badd =
        "{\"source\":\"x\",\"operation\":\"y\",\"status\":\"SUCCESS\",\"data\":[1,2]}";
    CHECK(tb_envelope_parse(badd, strlen(badd), &back, &err) == -1);
    CHECK(err.code == TB_JSON_SCHEMA);

    /* set_data с мусором — отказ. */
    tb_envelope_init(&env, "x", "y", 1);
    CHECK(tb_envelope_set_data(&env, "not json", &err) == -1);
    CHECK(err.code == TB_JSON_SCHEMA);

    printf("OK test_envelope\n");
    return 0;
}
