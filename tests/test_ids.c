/* ==========================================================================
 *  tests/test_ids.c — решётка идентификации: зоны ответственности.
 *
 *  Группа-владелец на ошибке (кто сообщил), владение кодов в реестре
 *  (канонический ответственный), имена групп, формат с группой.
 * ========================================================================== */
#define TB_CURRENT_GROUP TB_G_DEMO /* файл теста отвечает группой демо */

#include <stdio.h>
#include <string.h>

#include "harness.h"
#include "tb/core/error.h"
#include "tb/core/error_registry.h"
#include "tb/core/ids.h"
#include "tb/exec/codes.h"

int main(void)
{
    /* ---- имена групп ---- */
    CHECK_STR_EQ(tb_group_name(TB_G_DEX), "dex");
    CHECK_STR_EQ(tb_group_name(TB_G_FILTERS), "filters");
    CHECK_STR_EQ(tb_group_name(TB_G_UNASSIGNED), "unassigned");
    CHECK_STR_EQ(tb_group_name(9999), "g9999");

    /* ---- самотест реестра: у всех кодов есть владелец, дублей нет ---- */
    tb_error_t oerr = TB_ERROR_INIT;
    CHECK(tb_registry_ownership_check(&oerr) == 0);

    /* ---- канонические владельцы кодов ---- */
    const tb_error_meta_t *m;
    m = tb_registry_meta(TB_DOMAIN_USER, TB_EXEC_SLIPPAGE);
    CHECK(m != NULL && m->owner == TB_G_EXEC);
    m = tb_registry_meta(TB_DOMAIN_API, TB_API_RATE_LIMIT);
    CHECK(m != NULL && m->owner == TB_G_REGISTRY);
    m = tb_registry_meta(TB_DOMAIN_NET, TB_NET_TIMEOUT);
    CHECK(m != NULL && m->owner == TB_G_NET);
    m = tb_registry_meta(TB_DOMAIN_EVENTS, TB_EV_CONTRACT);
    CHECK(m != NULL && m->owner == TB_G_EVENTS);

    /* ---- атрибуция через макрос: группа файла-владельца ---- */
    tb_error_t err = TB_ERROR_INIT;
    TB_ERROR_SET(&err, TB_DOMAIN_USER, TB_EXEC_SLIPPAGE, "тест");
    CHECK(err.group == TB_G_DEMO);
    CHECK(err.domain == TB_DOMAIN_USER);
    CHECK(err.code == TB_EXEC_SLIPPAGE);

    /* ---- формат несёт имя группы ---- */
    char buf[256];
    tb_error_format(&err, buf, sizeof(buf));
    CHECK(strstr(buf, "USER:demo") != NULL);

    /* ---- wrap: группа переходит к обогатившему ---- */
    tb_error_wrap_g(&err, TB_G_EXEC, TB_LOC, "конвейер");
    CHECK(err.group == TB_G_EXEC);
    tb_error_format(&err, buf, sizeof(buf));
    CHECK(strstr(buf, "USER:exec") != NULL);
    CHECK(strstr(buf, "конвейер") != NULL);

    /* ---- макрос wrap берёт группу файла ---- */
    TB_ERROR_WRAP(&err, "макрос");
    CHECK(err.group == TB_G_DEMO);

    /* ---- promote: новая ошибка — новая группа ---- */
    tb_error_promote_g(&err, TB_G_DECISION, TB_DOMAIN_USER,
                       TB_EXEC_BACKEND, TB_LOC, "решение");
    CHECK(err.group == TB_G_DECISION);
    tb_error_format(&err, buf, sizeof(buf));
    CHECK(strstr(buf, "USER:decision") != NULL);
    CHECK(strstr(buf, "<-") != NULL); /* прежнее сообщение в следе */

    /* ---- старый API без группы: UNASSIGNED, формат без группы ---- */
    tb_error_t old = TB_ERROR_INIT;
    tb_error_set(&old, TB_DOMAIN_CORE, TB_CORE_IO, TB_LOC, "старый путь");
    CHECK(old.group == TB_G_UNASSIGNED);
    tb_error_format(&old, buf, sizeof(buf));
    CHECK(strstr(buf, "CORE/TB_CORE_IO") != NULL);
    CHECK(strstr(buf, ":demo") == NULL);
    CHECK(strstr(buf, ":unassigned") == NULL);

    /* ---- POD: {0}-инициализация и копирование переносят группу ---- */
    tb_error_t copy = err;
    CHECK(copy.group == TB_G_DECISION);
    tb_error_clear(&copy);
    CHECK(copy.group == TB_G_UNASSIGNED);

    printf("OK test_ids\n");
    return 0;
}
