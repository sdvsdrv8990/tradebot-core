#define TB_CURRENT_GROUP TB_G_JSON /* зона ответственности файла */
/* ==========================================================================
 *  tb/json/envelope.cpp — реализация Envelope на rapidjson (C-ABI).
 * ========================================================================== */
#include "tb/json/envelope.h"

#include <cstdio>
#include <cstring>
#include <string>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include "tb/core/error_registry.h"
#include "tb/core/time.h"

static void set_json_err(tb_error_t *err, uint32_t code, const char *msg)
{
    tb_error_set(err, TB_DOMAIN_JSON, code, "envelope", "%s", msg);
}

const char *tb_env_status_name(int status)
{
    switch (status) {
    case TB_ENV_SUCCESS: return "SUCCESS";
    case TB_ENV_ERROR:   return "ERROR";
    case TB_ENV_PENDING: return "PENDING";
    default:             return "?";
    }
}

void tb_envelope_init(tb_envelope_t *env, const char *source,
                      const char *operation, uint64_t request_id)
{
    if (!env) return;
    memset(env, 0, sizeof(*env));
    snprintf(env->source, sizeof(env->source), "%s", source ? source : "?");
    snprintf(env->operation, sizeof(env->operation), "%s", operation ? operation : "?");
    env->status = TB_ENV_SUCCESS;
    env->request_id = request_id;
    env->timestamp = (int64_t)tb_wall_ms();
    env->next_attempt_at = -1;
}

int tb_envelope_set_data(tb_envelope_t *env, const char *data_json, tb_error_t *err)
{
    if (!env) return -1;
    if (!data_json || !data_json[0]) {
        env->data_json[0] = '\0';
        tb_error_clear(err);
        return 0;
    }
    rapidjson::Document d;
    d.Parse(data_json, strlen(data_json));
    if (d.HasParseError() || !d.IsObject()) {
        set_json_err(err, TB_JSON_SCHEMA,
                     "data должен быть JSON-объектом или null");
        return -1;
    }
    size_t need = strlen(data_json);
    if (need >= sizeof(env->data_json)) {
        set_json_err(err, TB_JSON_BUFFER, "data не влезает в буфер Envelope");
        return -1;
    }
    memcpy(env->data_json, data_json, need + 1);
    tb_error_clear(err);
    return 0;
}

int tb_envelope_set_error(tb_envelope_t *env, const tb_error_t *error)
{
    if (!env || !error) return -1;
    /* tb_error_t — POD: копия присваиванием, без цепочки владения. */
    env->error = *error;
    env->status = TB_ENV_ERROR;
    return 0;
}

void tb_envelope_set_retry(tb_envelope_t *env, uint32_t attempt,
                           int64_t next_attempt_at)
{
    if (!env) return;
    env->attempt = attempt;
    env->next_attempt_at = next_attempt_at;
}

int tb_envelope_serialize(const tb_envelope_t *env, char *buf, size_t cap,
                          size_t *out_len, tb_error_t *err)
{
    if (!env || !buf || cap == 0) {
        set_json_err(err, TB_CORE_INVALID_ARG, "serialize: нулевые аргументы");
        return -1;
    }

    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);

    w.StartObject();
    w.Key("schema_version"); w.String(TB_ENV_SCHEMA_VERSION);
    w.Key("source");         w.String(env->source);
    w.Key("operation");      w.String(env->operation);
    w.Key("status");         w.String(tb_env_status_name(env->status));
    w.Key("request_id");     w.Uint64(env->request_id);
    w.Key("timestamp");      w.Int64(env->timestamp);

    if (env->data_json[0]) {
        /* data уже валидирован set_data/parse как JSON-объект. */
        w.Key("data");
        w.RawValue(env->data_json, strlen(env->data_json), rapidjson::kObjectType);
    } else {
        w.Key("data"); w.Null();
    }

    if (env->status == TB_ENV_ERROR) {
        const tb_error_meta_t *meta =
            tb_registry_meta(env->error.domain, env->error.code);
        w.Key("error");
        w.StartObject();
        w.Key("code"); w.String(meta ? meta->name : "TB_UNKNOWN");
        w.Key("domain"); w.String(tb_domain_name(env->error.domain));
        w.Key("severity"); w.String(tb_severity_name((tb_severity_t)env->error.severity));
        w.Key("retryable"); w.Bool(env->error.retryable);
        if (env->error.http_status != 0) {
            w.Key("http_status"); w.Int(env->error.http_status);
        }
        if (env->error.retry_after_ms != 0) {
            w.Key("retry_after_ms"); w.Uint(env->error.retry_after_ms);
        }
        w.Key("message"); w.String(env->error.message[0] ? env->error.message : "-");
        w.EndObject();
    } else {
        w.Key("error"); w.Null();
    }

    if (env->attempt > 0 || env->next_attempt_at >= 0) {
        w.Key("retry");
        w.StartObject();
        w.Key("attempt"); w.Uint(env->attempt);
        if (env->next_attempt_at >= 0) {
            w.Key("next_attempt_at"); w.Int64(env->next_attempt_at);
        } else {
            w.Key("next_attempt_at"); w.Null();
        }
        w.EndObject();
    } else {
        w.Key("retry"); w.Null();
    }
    w.EndObject();

    size_t len = sb.GetSize();
    if (len + 1 > cap) {
        set_json_err(err, TB_JSON_BUFFER, "буфер сериализации слишком мал");
        return -1;
    }
    memcpy(buf, sb.GetString(), len + 1);
    if (out_len) *out_len = len;
    tb_error_clear(err);
    return 0;
}

int tb_envelope_parse(const char *json, size_t len, tb_envelope_t *out,
                      tb_error_t *err)
{
    if (!json || !out) {
        set_json_err(err, TB_CORE_INVALID_ARG, "parse: нулевые аргументы");
        return -1;
    }
    rapidjson::Document d;
    d.Parse(json, len);
    if (d.HasParseError()) {
        set_json_err(err, TB_JSON_PARSE, "синтаксическая ошибка JSON");
        return -1;
    }
    if (!d.IsObject()) {
        set_json_err(err, TB_JSON_SCHEMA, "Envelope должен быть JSON-объектом");
        return -1;
    }

    tb_envelope_t env;
    memset(&env, 0, sizeof(env));
    env.next_attempt_at = -1;

    if (!d.HasMember("source") || !d["source"].IsString() ||
        !d.HasMember("operation") || !d["operation"].IsString() ||
        !d.HasMember("status") || !d["status"].IsString()) {
        set_json_err(err, TB_JSON_SCHEMA,
                     "Envelope: обязательны source, operation, status");
        return -1;
    }
    snprintf(env.source, sizeof(env.source), "%s", d["source"].GetString());
    snprintf(env.operation, sizeof(env.operation), "%s", d["operation"].GetString());

    const char *st = d["status"].GetString();
    if (strcmp(st, "SUCCESS") == 0)      env.status = TB_ENV_SUCCESS;
    else if (strcmp(st, "ERROR") == 0)   env.status = TB_ENV_ERROR;
    else if (strcmp(st, "PENDING") == 0) env.status = TB_ENV_PENDING;
    else {
        set_json_err(err, TB_JSON_SCHEMA, "Envelope: неизвестный status");
        return -1;
    }

    if (d.HasMember("request_id") && d["request_id"].IsUint64())
        env.request_id = d["request_id"].GetUint64();
    if (d.HasMember("timestamp") && d["timestamp"].IsInt64())
        env.timestamp = d["timestamp"].GetInt64();

    /* data: null -> пусто; объект -> компактифицируем обратно. */
    if (d.HasMember("data") && !d["data"].IsNull()) {
        if (!d["data"].IsObject()) {
            set_json_err(err, TB_JSON_SCHEMA, "Envelope.data должен быть объектом или null");
            return -1;
        }
        rapidjson::StringBuffer sb;
        rapidjson::Writer<rapidjson::StringBuffer> w(sb);
        d["data"].Accept(w);
        if (sb.GetSize() >= sizeof(env.data_json)) {
            set_json_err(err, TB_JSON_BUFFER, "data не влезает в буфер Envelope");
            return -1;
        }
        memcpy(env.data_json, sb.GetString(), sb.GetSize() + 1);
    }

    /* error */
    if (d.HasMember("error") && !d["error"].IsNull()) {
        const rapidjson::Value &e = d["error"];
        if (!e.IsObject() || !e.HasMember("code") || !e["code"].IsString()) {
            set_json_err(err, TB_JSON_SCHEMA, "Envelope.error: обязательное поле code");
            return -1;
        }
        uint8_t domain;
        uint32_t code;
        if (!tb_registry_code_by_name(e["code"].GetString(), &domain, &code)) {
            set_json_err(err, TB_JSON_SCHEMA, "Envelope.error: код не зарегистрирован");
            return -1;
        }
        env.error.domain = domain;
        env.error.code = code;
        tb_registry_decorate(&env.error); /* severity, retryable по реестру */
        if (e.HasMember("http_status") && e["http_status"].IsInt())
            env.error.http_status = e["http_status"].GetInt();
        if (e.HasMember("retry_after_ms") && e["retry_after_ms"].IsUint64())
            env.error.retry_after_ms = (uint32_t)e["retry_after_ms"].GetUint64();
        if (e.HasMember("retryable") && e["retryable"].IsBool())
            env.error.retryable = e["retryable"].GetBool();
        if (e.HasMember("message") && e["message"].IsString())
            snprintf(env.error.message, sizeof(env.error.message), "%s",
                     e["message"].GetString());
    }

    /* retry */
    if (d.HasMember("retry") && !d["retry"].IsNull()) {
        const rapidjson::Value &r = d["retry"];
        if (r.IsObject()) {
            if (r.HasMember("attempt") && r["attempt"].IsUint())
                env.attempt = r["attempt"].GetUint();
            if (r.HasMember("next_attempt_at")) {
                if (r["next_attempt_at"].IsInt64())
                    env.next_attempt_at = r["next_attempt_at"].GetInt64();
                else if (r["next_attempt_at"].IsNull())
                    env.next_attempt_at = -1;
            }
        }
    }

    *out = env;
    tb_error_clear(err);
    return 0;
}
