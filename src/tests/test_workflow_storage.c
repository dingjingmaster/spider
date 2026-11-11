#include "storage/private/storage_internal.h"
#include <pthread.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int64_t scalar(Store *s, const char *sql)
{
    sqlite3_stmt *q = NULL;
    assert(sqlite3_prepare_v2(s->db, sql, -1, &q, NULL) == SQLITE_OK);
    assert(sqlite3_step(q) == SQLITE_ROW);
    int64_t result = sqlite3_column_int64(q, 0);
    sqlite3_finalize(q);
    return result;
}
static void migration(void)
{
    Store s;
    char e[SP_ERROR] = "";
    assert(store_open(&s, ":memory:", e, sizeof(e)) == 0);
    /* Fresh stores use the current catalog migration version. */
    assert(scalar(&s, "PRAGMA user_version") == 4);
    assert(scalar(&s, "SELECT count(*) FROM sessions") == 0);
    store_close(&s);
}

static int64_t id(json_object *o, const char *key)
{
    return json_object_get_int64(jg(o, key));
}
static json_object *parse(const char *raw)
{
    json_object *v = json_tokener_parse(raw);
    assert(v);
    return v;
}
static int64_t start(Store *s, int max)
{
    char e[SP_ERROR] = "", raw[512];
    snprintf(raw, sizeof(raw),
             "{\"schema_version\":2,\"name\":\"test\",\"entry\":\"root\",\"vars\":{\"token\":9},"
             "\"max_steps\":%d,\"steps\":[]}",
             max);
    json_object *r = parse(raw);
    int64_t spider = store_save_spider(s, 0, r, e, sizeof(e));
    json_object_put(r);
    assert(spider > 0);
    int64_t run = store_start_run(s, spider, e, sizeof(e));
    assert(run > 0);
    assert(wf_initialize(s, e, sizeof(e)) == 0);
    assert(wf_initialize(s, e, sizeof(e)) == 0);
    return run;
}
static json_object *claim(Store *s)
{
    char e[SP_ERROR] = "old error";
    json_object *j = wf_claim(s, e, sizeof(e));
    if (!j)
        fprintf(stderr, "claim: %s\n", e);
    assert(j && !*e);
    assert(json_object_is_type(jg(j, "context"), json_type_object));
    assert(ji(jg(j, "snapshot"), "schema_version", 0) == 2);
    return j;
}
static void finish(Store *s, json_object *j, const char *out, const char *children,
                   const char *emit)
{
    char e[SP_ERROR] = "";
    json_object *o = parse(out), *c = parse(children), *v = emit ? parse(emit) : NULL;
    int rc = wf_finish(s, j, o, jg(j, "context"), c, v, false, e, sizeof(e));
    if (rc)
        fprintf(stderr, "finish: %s\n", e);
    assert(rc == 0);
    json_object_put(o);
    json_object_put(c);
    json_object_put(v);
}
static void groups_and_emit(void)
{
    Store s;
    char e[SP_ERROR] = "";
    assert(!store_open(&s, ":memory:", e, sizeof(e)));
    start(&s, 100);
    json_object *root = claim(&s);
    assert(scalar(&s, "SELECT count(*) FROM step_instances") == 1);
    assert(ji(jg(jg(root, "context"), "run"), "token", 0) == 9);
    json_object *children = parse("[{\"step_id\":\"a\",\"context\":{\"input\":1},\"record_key\":"
                                  "\"a\",\"branch_key\":\"alpha\"},{\"step_id\":\"b\",\"context\":{"
                                  "\"input\":2},\"record_key\":\"b\",\"branch_key\":\"beta\"}]");
    assert(!wf_fork(&s, root, children, jg(root, "context"), e, sizeof(e)));
    json_object_put(children);
    json_object *a = claim(&s), *b = claim(&s);
    finish(&s, a, "{\"temporary\":1}",
           "[{\"step_id\":\"end\",\"context\":{\"input\":3},\"record_key\":\"a\"}]", NULL);
    finish(&s, b, "{\"value\":2}", "[]", "{\"value\":2}");
    assert(scalar(&s, "SELECT count(*) FROM runs WHERE status='running'") == 1);
    json_object *end = claim(&s);
    finish(&s, end, "{\"value\":3}", "[]", "{\"value\":3}");
    finish(&s, end, "{\"value\":3}", "[]", "{\"value\":3}");
    json_object *resume = claim(&s);
    assert(ji(resume, "phase", 0) == 1);
    json_object *results =
        wf_group_results(&s, json_object_get_int64(jg(root, "id")), e, sizeof(e));
    assert(results && ji(jg(results, "alpha"), "value", 0) == 3);
    assert(ji(jg(results, "beta"), "value", 0) == 2);
    finish(&s, resume, "{}", "[]", NULL);
    assert(scalar(&s, "SELECT count(*) FROM runs WHERE status='succeeded'") == 1);
    assert(scalar(&s, "SELECT item_count FROM runs") == 2);
    assert(!wf_claim(&s, e, sizeof(e)) && !*e);
    json_object_put(results);
    json_object_put(root);
    json_object_put(a);
    json_object_put(b);
    json_object_put(end);
    json_object_put(resume);
    store_close(&s);
}
static void failures_and_limits(void)
{
    Store s;
    char e[SP_ERROR] = "";
    assert(!store_open(&s, ":memory:", e, sizeof(e)));
    int64_t run = start(&s, 2);
    json_object *root = claim(&s);
    json_object *children = parse("[{\"step_id\":\"a\",\"context\":{},\"branch_key\":\"a\"},{"
                                  "\"step_id\":\"b\",\"context\":{},\"branch_key\":\"b\"}]");
    assert(wf_fork(&s, root, children, jg(root, "context"), e, sizeof(e)) < 0 && *e);
    assert(scalar(&s, "SELECT count(*) FROM step_instances") == 1);
    assert(!wf_failure(&s, root, "fail", "limit", false, 0, e, sizeof(e)));
    assert(scalar(&s, "SELECT count(*) FROM runs WHERE status='failed'") == 1);
    assert(!wf_cancel(&s, run, e, sizeof(e)));
    json_object_put(root);
    json_object_put(children);
    start(&s, 20);
    root = claim(&s);
    assert(!wf_failure(&s, root, "fail", "transient", true, 0, e, sizeof(e)));
    json_object *retry = claim(&s);
    assert(ji(retry, "attempts", 0) == 2);
    assert(wf_finish(&s, root, NULL, jg(root, "context"), NULL, NULL, false, e, sizeof(e)) < 0);
    strcpy(e, "missing");
    assert(!wf_failure(&s, retry, "skip", e, false, 0, e, sizeof(e)));
    assert(
        scalar(&s,
               "SELECT count(*) FROM step_instances WHERE status='skipped' AND error='missing'") ==
        1);
    assert(scalar(&s, "SELECT count(*) FROM runs WHERE status='partial'") == 1);
    json_object_put(root);
    json_object_put(retry);
    run = start(&s, 20);
    root = claim(&s);
    assert(!wf_cancel(&s, run, e, sizeof(e)));
    assert(wf_finish(&s, root, NULL, jg(root, "context"), NULL, NULL, false, e, sizeof(e)) < 0);
    assert(scalar(&s, "SELECT count(*) FROM runs WHERE status='cancelled'") == 1);
    json_object_put(root);
    store_close(&s);
}

static void legacy_migration(void)
{
    char path[] = "/tmp/spider-wf-v1-XXXXXX", e[SP_ERROR] = "";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    sqlite3 *db;
    assert(sqlite3_open(path, &db) == SQLITE_OK);
    assert(sqlite3_exec(
               db,
               "PRAGMA user_version=1;"
               "CREATE TABLE spiders(id INTEGER PRIMARY KEY,name TEXT NOT NULL,rule_json TEXT NOT "
               "NULL,version INTEGER NOT NULL DEFAULT 1,enabled INTEGER NOT NULL DEFAULT "
               "1,interval_seconds INTEGER NOT NULL DEFAULT 0,next_run INTEGER NOT NULL DEFAULT "
               "0,deleted INTEGER NOT NULL DEFAULT 0,updated_at INTEGER NOT NULL);"
               "CREATE TABLE runs(id INTEGER PRIMARY KEY,spider_id INTEGER NOT NULL REFERENCES "
               "spiders(id),version INTEGER NOT NULL,snapshot TEXT NOT NULL,status TEXT NOT NULL "
               "DEFAULT 'queued',cancel_requested INTEGER NOT NULL DEFAULT 0,error TEXT NOT NULL "
               "DEFAULT '',pages INTEGER NOT NULL DEFAULT 0,item_count INTEGER NOT NULL DEFAULT "
               "0,created_at INTEGER NOT NULL,started_at INTEGER,finished_at INTEGER);"
               "CREATE TABLE requests(id INTEGER PRIMARY KEY,run_id INTEGER REFERENCES "
               "runs(id),url TEXT NOT NULL,status TEXT NOT NULL DEFAULT 'queued',attempts INTEGER "
               "NOT NULL DEFAULT 0,error TEXT NOT NULL DEFAULT '',UNIQUE(run_id,url));"
               "CREATE TABLE items(id INTEGER PRIMARY KEY,run_id INTEGER REFERENCES "
               "runs(id),request_id INTEGER NOT NULL REFERENCES requests(id),source_url TEXT NOT "
               "NULL,data TEXT NOT NULL,created_at INTEGER NOT NULL,UNIQUE(request_id,data));"
               "INSERT INTO spiders(id,name,rule_json,updated_at) VALUES(1,'old',"
               "'{\"name\":\"old\",\"url\":\"http://old/\",\"format\":\"json\",\"row_selector\":\"/"
               "rows\",\"fields\":[{\"name\":\"old\",\"selector\":\"/old\"}]}',1);"
               "INSERT INTO runs(id,spider_id,version,snapshot,status,pages,item_count,created_at) "
               "VALUES(1,1,1,'{}','succeeded',1,1,1);"
               "INSERT INTO requests(id,run_id,url,status,attempts) "
               "VALUES(7,1,'http://old','succeeded',1);"
               "INSERT INTO items(id,run_id,request_id,source_url,data,created_at) "
               "VALUES(9,1,7,'http://old','{\"old\":1}',1);",
               NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_close(db);
    /* Invalid current rules also roll back the earlier v1 table rebuild. */
    assert(sqlite3_open(path, &db) == SQLITE_OK);
    assert(!sqlite3_exec(db,
                         "INSERT INTO spiders(id,name,rule_json,updated_at) "
                         "VALUES(2,'invalid','{\"name\":\"invalid\"}',1)",
                         NULL, NULL, NULL));
    sqlite3_close(db);
    Store s;
    assert(store_open(&s, path, e, sizeof(e)) < 0 && *e);
    assert(sqlite3_open(path, &db) == SQLITE_OK);
    s.db = db; /* Read-only fixture checks use the scalar helper only. */
    assert(scalar(&s, "PRAGMA user_version") == 1);
    assert(
        scalar(&s,
               "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='step_instances'") ==
        0);
    assert(scalar(&s, "SELECT request_id FROM items WHERE id=9") == 7);
    assert(scalar(&s, "SELECT version FROM spiders WHERE id=1") == 1);
    assert(!sqlite3_exec(db, "DELETE FROM spiders WHERE id=2", NULL, NULL, NULL));
    sqlite3_close(db);
    s.db = NULL;
    assert(!store_open(&s, path, e, sizeof(e)));
    assert(scalar(&s, "PRAGMA user_version") == 4);
    assert(scalar(&s, "SELECT count(*) FROM spiders WHERE id=1 AND version=2 AND "
                      "json_extract(rule_json,'$.schema_version')=2") == 1);
    assert(scalar(&s, "SELECT count(*) FROM runs WHERE id=1 AND snapshot='{}' AND version=1 AND "
                      "status='succeeded'") == 1);
    assert(scalar(&s, "SELECT request_id FROM items WHERE id=9") == 7);
    assert(scalar(&s, "SELECT json_extract(data,'$.old') FROM items") == 1);
    assert(scalar(&s, "SELECT item_count FROM runs WHERE id=1") == 1);
    assert(sqlite3_exec(s.db, "INSERT INTO requests(run_id,url) VALUES(1,'http://old')", NULL, NULL,
                        NULL) == SQLITE_CONSTRAINT);
    assert(!sqlite3_exec(s.db,
                         "INSERT INTO requests(run_id,url,step_instance_id) "
                         "VALUES(1,'http://old',3),(1,'http://old',4)",
                         NULL, NULL, NULL));
    assert(!sqlite3_exec(
        s.db,
        "INSERT INTO items(run_id,source_url,data,created_at,step_instance_id,step_id,record_key) "
        "VALUES(1,'','{}',1,3,'emit','record')",
        NULL, NULL, NULL));
    sqlite3_stmt *q = NULL;
    assert(sqlite3_prepare_v2(s.db, "PRAGMA foreign_key_check", -1, &q, NULL) == SQLITE_OK);
    assert(sqlite3_step(q) == SQLITE_DONE);
    sqlite3_finalize(q);
    store_close(&s);
    unlink(path);
}
static void recovery(void)
{
    char path[] = "/tmp/spider-wf-recovery-XXXXXX", e[SP_ERROR] = "";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    Store s;
    assert(!store_open(&s, path, e, sizeof(e)));
    start(&s, 100);
    json_object *root = claim(&s),
                *children = parse("[{\"step_id\":\"a\",\"context\":{},\"branch_key\":\"a\"},{"
                                  "\"step_id\":\"b\",\"context\":{},\"branch_key\":\"b\"}]");
    assert(!wf_fork(&s, root, children, jg(root, "context"), e, sizeof(e)));
    json_object *a = claim(&s), *b = claim(&s);
    finish(&s, a, "{\"done\":1}", "[]", NULL);
    int64_t pending_id = id(b, "id");
    store_close(&s);
    assert(!store_open(&s, path, e, sizeof(e)));
    assert(!wf_initialize(&s, e, sizeof(e)));
    json_object *resumed = claim(&s);
    assert(id(resumed, "id") == pending_id && ji(resumed, "attempts", 0) == 2);
    assert(scalar(&s, "SELECT count(*) FROM step_instances") == 3);
    finish(&s, resumed, "{\"done\":2}", "[]", NULL);
    json_object *group = claim(&s);
    assert(id(group, "id") == id(root, "id") && ji(group, "phase", 0) == 1);
    finish(&s, group, "{}", "[]", NULL);
    assert(scalar(&s, "SELECT count(*) FROM runs WHERE status='succeeded'") == 1);
    json_object_put(root);
    json_object_put(children);
    json_object_put(a);
    json_object_put(b);
    json_object_put(resumed);
    json_object_put(group);
    start(&s, 10);
    root = claim(&s);
    assert(!store_cancel_run(&s, id(root, "run_id")));
    /* Existing API cancellation must immediately terminalize workflow runs. */
    assert(scalar(&s, "SELECT count(*) FROM runs WHERE status='running'") == 0);
    store_close(&s);
    assert(!store_open(&s, path, e, sizeof(e)));
    assert(!wf_initialize(&s, e, sizeof(e)));
    assert(!wf_claim(&s, e, sizeof(e)) && !*e);
    json_object_put(root);
    store_close(&s);
    unlink(path);
}

typedef struct {
    Store *store;
    json_object *job;
    char error[SP_ERROR];
} ClaimWorker;
static void *claim_worker(void *arg)
{
    ClaimWorker *worker = arg;
    worker->job = wf_claim(worker->store, worker->error, sizeof(worker->error));
    return NULL;
}
static void concurrent_and_nested(void)
{
    Store s;
    char e[SP_ERROR] = "";
    assert(!store_open(&s, ":memory:", e, sizeof(e)));
    start(&s, 100);
    json_object *root = claim(&s),
                *children = parse("[{\"step_id\":\"a\",\"context\":{},\"branch_key\":\"a\"},{"
                                  "\"step_id\":\"b\",\"context\":{},\"branch_key\":\"b\"}]");
    assert(!wf_fork(&s, root, children, jg(root, "context"), e, sizeof(e)));
    ClaimWorker workers[2] = {{.store = &s}, {.store = &s}};
    pthread_t threads[2];
    for (int i = 0; i < 2; i++)
        assert(!pthread_create(&threads[i], NULL, claim_worker, &workers[i]));
    for (int i = 0; i < 2; i++)
        assert(!pthread_join(threads[i], NULL));
    assert(workers[0].job && workers[1].job && !*workers[0].error && !*workers[1].error);
    assert(id(workers[0].job, "id") != id(workers[1].job, "id"));
    assert(!wf_fork(&s, workers[0].job, NULL, jg(workers[0].job, "context"), e, sizeof(e)));
    json_object *empty = claim(&s);
    assert(ji(empty, "phase", 0) == 1);
    json_object *out = wf_group_results(&s, id(empty, "id"), e, sizeof(e));
    assert(out && json_object_object_length(out) == 0);
    finish(&s, empty, "{\"nested\":1}", "[]", NULL);
    assert(scalar(&s, "SELECT count(*) FROM runs WHERE status='running'") == 1);
    finish(&s, workers[1].job, "{\"other\":2}", "[]", NULL);
    json_object *group = claim(&s);
    finish(&s, group, "{}", "[]", NULL);
    assert(scalar(&s, "SELECT count(*) FROM runs WHERE status='succeeded'") == 1);
    json_object_put(root);
    json_object_put(children);
    json_object_put(empty);
    json_object_put(out);
    json_object_put(workers[0].job);
    json_object_put(workers[1].job);
    json_object_put(group);
    store_close(&s);
}
static void fanout_context_and_database_errors(void)
{
    Store s;
    char e[SP_ERROR] = "";
    assert(!store_open(&s, ":memory:", e, sizeof(e)));
    start(&s, 10000);
    assert(!sqlite3_exec(
        s.db,
        "CREATE TRIGGER deny_claim BEFORE UPDATE OF status ON step_instances WHEN "
        "NEW.status='running' BEGIN SELECT RAISE(ABORT,'injected claim failure'); END",
        NULL, NULL, NULL));
    assert(!wf_claim(&s, e, sizeof(e)) && *e);
    assert(!sqlite3_exec(s.db, "DROP TRIGGER deny_claim", NULL, NULL, NULL));
    json_object *root = claim(&s), *children = json_object_new_array();
    for (int i = 0; i < 1001; i++)
        json_object_array_add(children, parse("{\"step_id\":\"next\",\"context\":{}}"));
    assert(wf_fork(&s, root, children, jg(root, "context"), e, sizeof(e)) < 0 && *e);
    assert(scalar(&s, "SELECT count(*) FROM step_instances") == 1);
    json_object_put(children);
    json_object *large = json_object_new_object();
    char *bytes = malloc(4 * 1024 * 1024 + 1);
    assert(bytes);
    memset(bytes, 'x', 4 * 1024 * 1024);
    bytes[4 * 1024 * 1024] = 0;
    json_object_object_add(large, "large", json_object_new_string(bytes));
    free(bytes);
    assert(wf_finish(&s, root, NULL, large, NULL, NULL, false, e, sizeof(e)) < 0 && *e);
    json_object_put(large);
    assert(!sqlite3_exec(s.db,
                         "CREATE TRIGGER deny_emit BEFORE INSERT ON items BEGIN SELECT "
                         "RAISE(ABORT,'injected emit failure'); END",
                         NULL, NULL, NULL));
    json_object *emit = parse("{\"value\":1}"),
                *next = parse("[{\"step_id\":\"next\",\"context\":{}}]");
    assert(wf_finish(&s, root, NULL, jg(root, "context"), next, emit, true, e, sizeof(e)) < 0 &&
           *e);
    assert(scalar(&s, "SELECT count(*) FROM step_instances") == 1);
    assert(scalar(&s, "SELECT pages FROM runs") == 0);
    assert(!sqlite3_exec(s.db, "DROP TRIGGER deny_emit", NULL, NULL, NULL));
    assert(!wf_failure(&s, root, "continue", "warning", false, 0, e, sizeof(e)));
    assert(!wf_finish(&s, root, NULL, jg(root, "context"), NULL, emit, true, e, sizeof(e)));
    assert(scalar(&s, "SELECT pages FROM runs") == 1);
    assert(scalar(&s, "SELECT count(*) FROM runs WHERE status='partial'") == 1);
    json_object_put(root);
    json_object_put(emit);
    json_object_put(next);
    store_close(&s);
}

static void idempotent_record_and_snapshot(void)
{
    Store s;
    char e[SP_ERROR] = "";
    assert(!store_open(&s, ":memory:", e, sizeof(e)));
    start(&s, 10);
    json_object *root = claim(&s);
    assert(!sqlite3_exec(s.db,
                         "UPDATE spiders SET "
                         "rule_json='{\"schema_version\":2,\"name\":\"edited\"}',version=version+1",
                         NULL, NULL, NULL));
    json_object_object_add(jg(root, "context"), "source_url",
                           json_object_new_string("http://source/page"));
    finish(&s, root, "{}", "[{\"step_id\":\"root\",\"context\":{}}]", "{\"value\":1}");
    json_object *second = claim(&s);
    assert(!strcmp(js(jg(second, "snapshot"), "name", ""), "test"));
    finish(&s, second, "{}", "[]", "{\"value\":2}");
    assert(scalar(&s, "SELECT item_count FROM runs") == 1);
    assert(scalar(&s, "SELECT count(*) FROM items WHERE source_url='http://source/page'") == 1);
    assert(scalar(&s, "SELECT json_extract(data,'$.value') FROM items") == 1);
    json_object_put(root);
    json_object_put(second);
    store_close(&s);
}
static void group_output_bound(void)
{
    Store s;
    char e[SP_ERROR] = "";
    assert(!store_open(&s, ":memory:", e, sizeof(e)));
    start(&s, 10);
    json_object *root = claim(&s),
                *children = parse("[{\"step_id\":\"a\",\"context\":{},\"branch_key\":\"a\"},{"
                                  "\"step_id\":\"b\",\"context\":{},\"branch_key\":\"b\"}]");
    assert(!wf_fork(&s, root, children, jg(root, "context"), e, sizeof(e)));
    size_t bytes_count = 2200000;
    char *bytes = malloc(bytes_count + 1);
    assert(bytes);
    memset(bytes, 'x', bytes_count);
    bytes[bytes_count] = 0;
    json_object *output = json_object_new_string(bytes);
    free(bytes);
    for (int i = 0; i < 2; i++) {
        json_object *job = claim(&s);
        assert(!wf_finish(&s, job, output, jg(job, "context"), NULL, NULL, false, e, sizeof(e)));
        json_object_put(job);
    }
    json_object *resumed = claim(&s);
    json_object *result = wf_group_results(&s, id(root, "id"), e, sizeof(e));
    assert(!result && *e);
    assert(!wf_failure(&s, resumed, "fail", e, false, 0, NULL, 0));
    json_object_put(root);
    json_object_put(children);
    json_object_put(output);
    json_object_put(resumed);
    store_close(&s);
}

static void artifact_terminal_guard(void)
{
    Store s;
    char e[SP_ERROR] = "";
    assert(!store_open(&s, ":memory:", e, sizeof(e)));
    int64_t run = start(&s, 10);
    assert(store_add_artifact(&s, run, "http://artifact", "/tmp/first", "a", 10) > 0);
    assert(!wf_cancel(&s, run, e, sizeof(e)));
    assert(store_add_artifact(&s, run, "http://artifact", "/tmp/late", "b", 20) < 0);
    assert(store_add_artifact(&s, run, "http://another", "/tmp/late", "b", 20) < 0);
    assert(scalar(&s, "SELECT bytes FROM artifacts") == 10);
    store_close(&s);
}

static void session_recovery_cleanup(void)
{
    char path[] = "/tmp/spider-wf-session-XXXXXX", e[SP_ERROR] = "";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    Store s;
    assert(!store_open(&s, path, e, sizeof(e)));
    int64_t first = start(&s, 10);
    assert(!sqlite3_exec(s.db, "INSERT INTO sessions SELECT id,'cookie=active' FROM runs", NULL,
                         NULL, NULL));
    json_object *job = claim(&s);
    store_close(&s);
    assert(!store_open(&s, path, e, sizeof(e)));
    assert(scalar(&s, "SELECT count(*) FROM sessions WHERE cookies='cookie=active'") == 1);
    assert(!wf_cancel(&s, first, e, sizeof(e)));
    start(&s, 10);
    assert(!sqlite3_exec(
        s.db, "INSERT INTO sessions SELECT id,'cookie=second' FROM runs WHERE status='running'",
        NULL, NULL, NULL));
    store_close(&s);
    assert(!store_open(&s, path, e, sizeof(e)));
    assert(scalar(&s, "SELECT count(*) FROM sessions") == 1);
    assert(scalar(&s, "SELECT count(*) FROM sessions WHERE cookies='cookie=second'") == 1);
    json_object_put(job);
    store_close(&s);
    unlink(path);
}
int main(void)
{
    migration();
    groups_and_emit();
    failures_and_limits();
    legacy_migration();
    recovery();
    concurrent_and_nested();
    fanout_context_and_database_errors();
    idempotent_record_and_snapshot();
    group_output_bound();
    artifact_terminal_guard();
    session_recovery_cleanup();
    puts("workflow storage tests passed");
    return 0;
}
