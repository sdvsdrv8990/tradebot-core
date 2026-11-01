/* tests/test_ledger.c — журнал событий: запись, дедупликация, reopen. */
#include "harness.h"
#include <unistd.h>
#include "tb/ledger/ledger.h"

int main(void)
{
    char path[128];
    snprintf(path, sizeof(path), "/tmp/tb_test_ledger_%d.jsonl", (int)getpid());
    unlink(path);

    tb_error_t err = TB_ERROR_INIT;
    tb_ledger_t *led = tb_ledger_open(path, TB_LEDGER_FLUSH_LINE, &err);
    CHECK(led != NULL);

    uint64_t id = 0;
    CHECK(tb_ledger_emit(led, "test.event", "{\"n\":1}", NULL, &id, &err) == 0);
    CHECK(id == 1);
    CHECK(tb_ledger_emit(led, "test.event", "{\"n\":2}", NULL, &id, &err) == 0);
    CHECK(id == 2);

    /* Дедупликация по uuid: первый пишется, второй пропускается. */
    CHECK(tb_ledger_emit(led, "heartbeat", "{\"x\":1}", "u-1", &id, &err) == 0);
    CHECK(id == 3);
    CHECK(tb_ledger_emit(led, "heartbeat", "{\"x\":1}", "u-1", &id, &err) == 0);
    CHECK(id == 0); /* дубликат */
    CHECK(err.code == TB_LEDGER_DUP);
    CHECK(tb_ledger_stats(led)->events_written == 3);
    CHECK(tb_ledger_stats(led)->events_duplicate == 1);
    CHECK(tb_ledger_last_event_id(led) == 3);

    /* Некорректный payload. */
    CHECK(tb_ledger_emit(led, "bad", "not json", NULL, &id, &err) == -1);
    CHECK(err.code == TB_LEDGER_BAD_JSON);
    CHECK(tb_ledger_emit(led, "bad", "{\"a\":1}\n{\"b\":2}", NULL, &id, &err) == -1);
    CHECK(err.code == TB_LEDGER_BAD_JSON);
    CHECK(tb_ledger_emit(led, "bad", "", NULL, &id, &err) == -1);

    tb_ledger_close(led);

    /* ---- Повторное открытие: нумерация продолжается ---- */
    led = tb_ledger_open(path, 0, &err);
    CHECK(led != NULL);
    CHECK(tb_ledger_last_event_id(led) == 3);
    CHECK(tb_ledger_emit(led, "test.event", "{\"n\":4}", NULL, &id, &err) == 0);
    CHECK(id == 4);
    tb_ledger_close(led);

    /* ---- Содержимое файла ---- */
    FILE *fp = fopen(path, "r");
    CHECK(fp != NULL);
    char line[TB_LEDGER_MAX_LINE];
    int lines = 0;
    bool saw_id1 = false, saw_uuid = false;
    while (fgets(line, sizeof(line), fp)) {
        lines++;
        if (strstr(line, "\"event_id\":1,") && strstr(line, "\"type\":\"test.event\""))
            saw_id1 = true;
        if (strstr(line, "\"uuid\":\"u-1\""))
            saw_uuid = true;
        CHECK(strstr(line, "\"ts\":\"20") != NULL);   /* ISO-время */
        CHECK(strstr(line, "\"payload\":{") != NULL); /* payload на месте */
        CHECK(line[strlen(line) - 1] == '\n');
    }
    fclose(fp);
    CHECK(lines == 4);
    CHECK(saw_id1);
    CHECK(saw_uuid);
    unlink(path);

    /* Дубликат не пишется — строк в файле столько же, сколько событий. */
    printf("OK test_ledger\n");
    return 0;
}
