#include "storage/private/storage_internal.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int64_t scalar(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *q = NULL;
    assert(sqlite3_prepare_v2(db, sql, -1, &q, NULL) == SQLITE_OK);
    assert(sqlite3_step(q) == SQLITE_ROW);
    int64_t value = sqlite3_column_int64(q, 0);
    sqlite3_finalize(q);
    return value;
}
static char *text_value(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *q = NULL;
    assert(sqlite3_prepare_v2(db, sql, -1, &q, NULL) == SQLITE_OK);
    assert(sqlite3_step(q) == SQLITE_ROW);
    char *value = strdup((const char *)sqlite3_column_text(q, 0));
    assert(value);
    sqlite3_finalize(q);
    return value;
}
static json_object *parse(const char *raw)
{
    json_object *o = json_tokener_parse(raw);
    assert(o);
    return o;
}
static void regression(void)
{
    char path[] = "/tmp/spider-test-XXXXXX", err[SP_ERROR] = "";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    Store s;
    assert(!store_open(&s, path, err, sizeof(err)));
    json_object *rule =
        parse("{\"schema_version\":2,\"name\":\"first\",\"entry\":\"root\",\"steps\":[{\"id\":"
              "\"root\",\"type\":\"emit\",\"fields\":{\"x\":1}}]}");
    int64_t spider = store_save_spider(&s, 0, rule, err, sizeof(err));
    assert(spider > 0);
    int64_t run = store_start_run(&s, spider, err, sizeof(err));
    assert(run > 0 && store_start_run(&s, spider, err, sizeof(err)) < 0);
    json_object_object_add(rule, "name", json_object_new_string("edited"));
    assert(store_save_spider(&s, spider, rule, err, sizeof(err)) == spider);
    assert(!wf_initialize(&s, err, sizeof(err)));
    json_object *job = wf_claim(&s, err, sizeof(err));
    assert(job && !strcmp(js(jg(job, "snapshot"), "name", ""), "first"));
    store_close(&s);
    assert(!store_open(&s, path, err, sizeof(err)));
    assert(!wf_initialize(&s, err, sizeof(err)));
    json_object *resumed = wf_claim(&s, err, sizeof(err));
    assert(resumed && ji(resumed, "attempts", 0) == 2);
    assert(store_cancel_run(&s, run) == 0);
    assert(
        scalar(s.db, "SELECT count(*) FROM runs WHERE cancel_requested=1 AND status='cancelled'") ==
        1);
    assert(store_add_artifact(&s, run, "http://localhost/", "/tmp/rejected", "hash", 1) < 0);
    assert(store_start_run(&s, spider, err, sizeof(err)) > 0);
    json_object_put(job);
    json_object_put(resumed);
    json_object_put(rule);
    store_close(&s);
    unlink(path);
}
static const char *legacy_rule =
    "{\"name\":\"legacy\",\"url\":\"http://localhost/\",\"format\":\"json\",\"row_selector\":\"/"
    "rows\",\"fields\":[{\"name\":\"x\",\"selector\":\"/"
    "x\"}],\"interval_seconds\":60,\"enabled\":false}";
static void fixture(const char *path, bool invalid)
{
    Store s;
    char e[SP_ERROR] = "";
    assert(!store_open(&s, path, e, sizeof(e)));
    json_object *r = parse(legacy_rule);
    assert(store_save_spider(&s, 0, r, e, sizeof(e)) == 1);
    assert(store_start_run(&s, 1, e, sizeof(e)) == 1);
    assert(store_save_spider(&s, 0, r, e, sizeof(e)) == 2);
    assert(store_start_run(&s, 2, e, sizeof(e)) == 2);
    json_object_put(r);
    r = parse("{\"schema_version\":2,\"name\":\"existing\",\"entry\":\"root\",\"steps\":[{\"id\":"
              "\"root\",\"type\":\"emit\",\"fields\":{}}]}");
    assert(store_save_spider(&s, 0, r, e, sizeof(e)) == 3);
    json_object_put(r);
    assert(!sqlite3_exec(s.db,
                         "UPDATE runs SET status='running',pages=1,item_count=1 WHERE id=1;"
                         "INSERT INTO requests(id,run_id,url,status,attempts) "
                         "VALUES(7,1,'http://localhost/','running',2);"
                         "INSERT INTO items(id,run_id,request_id,source_url,data,created_at) "
                         "VALUES(9,1,7,'http://localhost/','{\"x\":3}',1);"
                         "PRAGMA user_version=2;",
                         NULL, NULL, NULL));
    if (invalid)
        assert(!sqlite3_exec(s.db,
                             "UPDATE spiders SET "
                             "rule_json='{\"name\":\"broken\",\"url\":\"file:///bad\"}' WHERE id=2",
                             NULL, NULL, NULL));
    store_close(&s);
}
static void version3_migration(void)
{
    char path[] = "/tmp/spider-v3-XXXXXX", e[SP_ERROR] = "";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    fixture(path, false);
    sqlite3 *original = NULL;
    assert(sqlite3_open(path, &original) == SQLITE_OK);
    char *snapshot = text_value(original, "SELECT snapshot FROM runs WHERE id=1");
    char *existing = text_value(original, "SELECT rule_json FROM spiders WHERE id=3");
    char *history =
        text_value(original, "SELECT rule_json FROM rule_versions WHERE spider_id=1 AND version=1");
    sqlite3_close(original);
    Store s;
    assert(!store_open(&s, path, e, sizeof(e)));
    char *after = text_value(s.db, "SELECT snapshot FROM runs WHERE id=1");
    assert(!strcmp(snapshot, after));
    free(after);
    free(snapshot);
    after = text_value(s.db, "SELECT rule_json FROM spiders WHERE id=3");
    assert(!strcmp(existing, after));
    free(after);
    free(existing);
    after = text_value(s.db, "SELECT rule_json FROM rule_versions WHERE spider_id=1 AND version=1");
    assert(!strcmp(history, after));
    free(after);
    free(history);
    assert(scalar(s.db, "PRAGMA user_version") == 3);
    assert(scalar(s.db, "SELECT count(*) FROM spiders WHERE id IN(1,2) AND version=2 AND "
                        "json_extract(rule_json,'$.schema_version')=2") == 2);
    assert(scalar(s.db, "SELECT count(*) FROM spiders WHERE id=3 AND version=1") == 1);
    assert(scalar(s.db, "SELECT count(*) FROM rule_versions") == 5);
    assert(scalar(s.db, "SELECT count(*) FROM rule_versions WHERE version=1 AND "
                        "json_extract(rule_json,'$.schema_version') IS NULL") == 2);
    assert(scalar(s.db, "SELECT count(*) FROM runs WHERE id IN(1,2) AND status='failed' AND error "
                        "LIKE '%migration%' AND finished_at IS NOT NULL") == 2);
    assert(scalar(s.db, "SELECT count(*) FROM runs WHERE version=1 AND "
                        "json_extract(snapshot,'$.schema_version') IS NULL") == 2);
    assert(scalar(s.db, "SELECT request_id FROM items WHERE id=9") == 7);
    assert(scalar(s.db, "SELECT json_extract(data,'$.x') FROM items WHERE id=9") == 3);
    assert(scalar(s.db,
                  "SELECT count(*) FROM requests WHERE id=7 AND attempts=2 AND status='running'") ==
           1);
    assert(scalar(s.db, "SELECT item_count FROM runs WHERE id=1") == 1);
    assert(scalar(s.db, "SELECT count(*) FROM spiders WHERE interval_seconds=60 AND enabled=0") ==
           2);
    store_close(&s);
    assert(!store_open(&s, path, e, sizeof(e)));
    assert(scalar(s.db, "SELECT count(*) FROM rule_versions") == 5);
    assert(scalar(s.db, "SELECT sum(version) FROM spiders") == 5);
    assert(store_start_run(&s, 1, e, sizeof(e)) > 2);
    store_close(&s);
    unlink(path);
}
static void invalid_rolls_back(void)
{
    char path[] = "/tmp/spider-v3-invalid-XXXXXX", e[SP_ERROR] = "";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    fixture(path, true);
    Store s;
    assert(store_open(&s, path, e, sizeof(e)) < 0 && *e);
    sqlite3 *db = NULL;
    assert(sqlite3_open(path, &db) == SQLITE_OK);
    assert(scalar(db, "PRAGMA user_version") == 2);
    assert(scalar(db, "SELECT count(*) FROM spiders WHERE version=1") == 3);
    assert(scalar(db, "SELECT count(*) FROM rule_versions") == 3);
    assert(scalar(db, "SELECT count(*) FROM runs WHERE status IN('queued','running')") == 2);
    assert(scalar(db, "SELECT count(*) FROM spiders WHERE id=1 AND "
                      "json_extract(rule_json,'$.schema_version') IS NULL") == 1);
    assert(scalar(db, "SELECT count(*) FROM items WHERE id=9") == 1);
    assert(
        !sqlite3_exec(db, "UPDATE spiders SET rule_json='{broken' WHERE id=2", NULL, NULL, NULL));
    sqlite3_close(db);
    assert(store_open(&s, path, e, sizeof(e)) < 0 && *e);
    assert(sqlite3_open(path, &db) == SQLITE_OK);
    assert(scalar(db, "PRAGMA user_version") == 2);
    assert(scalar(db, "SELECT count(*) FROM rule_versions") == 3);
    sqlite3_close(db);
    unlink(path);
}
static void native_item_atomicity(void)
{
    Store s;
    char e[SP_ERROR] = {0};
    assert(!store_open(&s, ":memory:", e, sizeof(e)));
    json_object *config = parse("{\"name\":\"native\",\"implementation\":\"fixture\"}");
    int64_t spider = store_save_spider(&s, 0, config, e, sizeof(e));
    assert(spider > 0);
    int64_t run = store_start_run(&s, spider, e, sizeof(e));
    assert(run > 0);
    json_object *claimed = store_claim_run(&s, NULL, 0, e, sizeof(e));
    assert(claimed);
    json_object_put(claimed);
    json_object *item = parse("{\"value\":7}");
    assert(!sqlite3_exec(s.db,
                         "CREATE TRIGGER deny_native_counter BEFORE UPDATE OF item_count ON runs "
                         "BEGIN SELECT RAISE(ABORT,'counter rejected'); END",
                         NULL, NULL, NULL));
    assert(store_emit(&s, run, "item", "native://fixture", item, e, sizeof(e)) < 0);
    assert(scalar(s.db, "SELECT count(*) FROM items") == 0);
    assert(!sqlite3_exec(s.db, "DROP TRIGGER deny_native_counter", NULL, NULL, NULL));
    assert(!store_emit(&s, run, "item", "native://fixture", item, e, sizeof(e)));
    assert(!store_emit(&s, run, "item", "native://fixture", item, e, sizeof(e)));
    assert(scalar(s.db, "SELECT count(*) FROM items") == 1);
    assert(scalar(s.db, "SELECT item_count FROM runs") == 1);
    assert(!store_cancel_run(&s, run));
    assert(store_emit(&s, run, "late", "native://fixture", item, e, sizeof(e)) < 0);
    assert(scalar(s.db, "SELECT count(*) FROM items") == 1);
    json_object_put(item);
    json_object_put(config);
    store_close(&s);
}
static void native_csv_export(void)
{
    Store s;
    char e[SP_ERROR] = {0};
    assert(!store_open(&s, ":memory:", e, sizeof(e)));
    const char *configs[] = {
        "{\"name\":\"native\",\"implementation\":\"fixture\"}",
        "{\"name\":\"native fields\",\"implementation\":\"fixture\",\"fields\":\"site-specific "
        "option\"}"};
    for (size_t i = 0; i < 2; i++) {
        json_object *config = parse(configs[i]);
        int64_t spider = store_save_spider(&s, 0, config, e, sizeof(e));
        assert(spider > 0);
        int64_t run = store_start_run(&s, spider, e, sizeof(e));
        assert(run > 0);
        json_object *claimed = store_claim_run(&s, NULL, 0, e, sizeof(e));
        assert(claimed);
        json_object_put(claimed);
        json_object *item = parse("{\"title\":\"hello\",\"value\":7}");
        assert(!store_emit(&s, run, "one", "native://fixture", item, e, sizeof(e)));
        char *body = NULL;
        size_t size = 0;
        assert(!store_export(&s, run, true, &body, &size, e, sizeof(e)));
        assert(!strcmp(body, "\"title\",\"value\"\r\n\"hello\",\"7\"\r\n"));
        assert(size == strlen(body));
        free(body);
        assert(!store_finish_run(&s, run, "succeeded", ""));
        json_object_put(config);
        json_object_put(item);
    }
    store_close(&s);
}
int main(void)
{
    native_csv_export();
    native_item_atomicity();
    regression();
    version3_migration();
    invalid_rolls_back();
    puts("storage tests passed");
    return 0;
}
