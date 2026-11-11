#include "private/storage_internal.h"
#include "rule/rule.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static const char *schema =
    "CREATE TABLE IF NOT EXISTS spiders(id INTEGER PRIMARY KEY,name TEXT NOT NULL,rule_json TEXT "
    "NOT NULL,version INTEGER NOT NULL DEFAULT 1,enabled INTEGER NOT NULL DEFAULT "
    "1,interval_seconds INTEGER NOT NULL DEFAULT 0,next_run INTEGER NOT NULL DEFAULT 0,deleted "
    "INTEGER NOT NULL DEFAULT 0,updated_at INTEGER NOT NULL);"
    "CREATE TABLE IF NOT EXISTS rule_versions(spider_id INTEGER NOT NULL REFERENCES "
    "spiders(id),version INTEGER NOT NULL,rule_json TEXT NOT NULL,created_at INTEGER NOT "
    "NULL,PRIMARY KEY(spider_id,version));"
    "CREATE TABLE IF NOT EXISTS runs(id INTEGER PRIMARY KEY,spider_id INTEGER NOT NULL REFERENCES "
    "spiders(id),version INTEGER NOT NULL,snapshot TEXT NOT NULL,status TEXT NOT NULL DEFAULT "
    "'queued',cancel_requested INTEGER NOT NULL DEFAULT 0,error TEXT NOT NULL DEFAULT '',pages "
    "INTEGER NOT NULL DEFAULT 0,item_count INTEGER NOT NULL DEFAULT 0,created_at INTEGER NOT "
    "NULL,started_at INTEGER,finished_at INTEGER);"
    "CREATE UNIQUE INDEX IF NOT EXISTS one_active_run ON runs(spider_id) WHERE status "
    "IN('queued','running');"
    "CREATE TABLE IF NOT EXISTS requests(id INTEGER PRIMARY KEY,run_id INTEGER NOT NULL REFERENCES "
    "runs(id),url TEXT NOT NULL,status TEXT NOT NULL DEFAULT 'queued',attempts INTEGER NOT NULL "
    "DEFAULT 0,error TEXT NOT NULL DEFAULT '',UNIQUE(run_id,url));"
    "CREATE TABLE IF NOT EXISTS items(id INTEGER PRIMARY KEY,run_id INTEGER NOT NULL REFERENCES "
    "runs(id),request_id INTEGER NOT NULL REFERENCES requests(id),source_url TEXT NOT NULL,data "
    "TEXT NOT NULL,created_at INTEGER NOT NULL,UNIQUE(request_id,data));"
    "CREATE TABLE IF NOT EXISTS artifacts(id INTEGER PRIMARY KEY,run_id INTEGER NOT NULL "
    "REFERENCES runs(id),url TEXT NOT NULL,path TEXT NOT NULL,sha256 TEXT NOT NULL,bytes INTEGER "
    "NOT NULL,UNIQUE(run_id,url));"
    "CREATE TABLE IF NOT EXISTS run_events(id INTEGER PRIMARY KEY,run_id INTEGER NOT NULL "
    "REFERENCES runs(id),level TEXT NOT NULL,message TEXT NOT NULL,created_at INTEGER NOT NULL);"
    "CREATE INDEX IF NOT EXISTS requests_queue ON requests(run_id,status,id);"
    "CREATE INDEX IF NOT EXISTS items_run ON items(run_id,id);";

/* Rebuild the two v1 tables together: foreign keys are disabled only while opening,
 * before this Store is visible to workers. Transaction rollback preserves v1. */
static const char *migration_v2 =
    "ALTER TABLE items RENAME TO items_v1;"
    "ALTER TABLE requests RENAME TO requests_v1;"
    "CREATE TABLE requests(id INTEGER PRIMARY KEY,run_id INTEGER NOT NULL REFERENCES runs(id),"
    "url TEXT NOT NULL,status TEXT NOT NULL DEFAULT 'queued',attempts INTEGER NOT NULL DEFAULT 0,"
    "error TEXT NOT NULL DEFAULT '',step_instance_id INTEGER,method TEXT,request_key TEXT);"
    "INSERT INTO requests(id,run_id,url,status,attempts,error) SELECT "
    "id,run_id,url,status,attempts,error FROM requests_v1;"
    "CREATE UNIQUE INDEX requests_legacy_url ON requests(run_id,url) WHERE step_instance_id IS "
    "NULL;"
    "CREATE UNIQUE INDEX requests_workflow_key ON requests(run_id,request_key) WHERE request_key "
    "IS NOT NULL;"
    "CREATE TABLE items(id INTEGER PRIMARY KEY,run_id INTEGER NOT NULL REFERENCES runs(id),"
    "request_id INTEGER REFERENCES requests(id),source_url TEXT NOT NULL DEFAULT '',data TEXT NOT "
    "NULL,"
    "created_at INTEGER NOT NULL,step_instance_id INTEGER,record_key TEXT,step_id "
    "TEXT,UNIQUE(request_id,data));"
    "INSERT INTO items(id,run_id,request_id,source_url,data,created_at) SELECT "
    "id,run_id,request_id,source_url,data,created_at FROM items_v1;"
    "DROP TABLE items_v1;DROP TABLE requests_v1;"
    "CREATE UNIQUE INDEX items_workflow_record ON items(run_id,record_key,step_id) WHERE "
    "step_instance_id IS NOT NULL;"
    "CREATE INDEX requests_queue ON requests(run_id,status,id);CREATE INDEX items_run ON "
    "items(run_id,id);"
    "CREATE TABLE step_instances(id INTEGER PRIMARY KEY,run_id INTEGER NOT NULL REFERENCES "
    "runs(id),"
    "step_id TEXT NOT NULL,parent_id INTEGER REFERENCES step_instances(id),branch_key TEXT NOT "
    "NULL DEFAULT '',"
    "record_key TEXT NOT NULL DEFAULT '',context TEXT NOT NULL,output TEXT,status TEXT NOT NULL "
    "DEFAULT 'queued',"
    "phase INTEGER NOT NULL DEFAULT 0,attempts INTEGER NOT NULL DEFAULT 0,pending INTEGER NOT NULL "
    "DEFAULT 0,"
    "terminal INTEGER NOT NULL DEFAULT 0,ready_at INTEGER NOT NULL DEFAULT 0,error TEXT NOT NULL "
    "DEFAULT '',"
    "created_at INTEGER NOT NULL,finished_at INTEGER);"
    "CREATE INDEX workflow_ready ON step_instances(status,ready_at,id);"
    "CREATE INDEX workflow_run ON step_instances(run_id,status);"
    "CREATE INDEX workflow_parent ON step_instances(parent_id);"
    "CREATE TABLE sessions(run_id INTEGER PRIMARY KEY REFERENCES runs(id),cookies TEXT NOT NULL "
    "DEFAULT '');"
    "PRAGMA user_version=2;";

static sqlite3_stmt *prepare(Store *s, const char *sql)
{
    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(s->db, sql, -1, &q, NULL) != SQLITE_OK)
        return NULL;
    return q;
}
static void txt(sqlite3_stmt *q, int i, const char *s)
{
    sqlite3_bind_text(q, i, s ? s : "", -1, SQLITE_TRANSIENT);
}
static json_object *row_json(sqlite3_stmt *q)
{
    json_object *o = json_object_new_object();
    for (int i = 0; i < sqlite3_column_count(q); i++) {
        const char *key = sqlite3_column_name(q, i);
        json_object *v = NULL;
        int type = sqlite3_column_type(q, i);
        if (type == SQLITE_INTEGER)
            v = json_object_new_int64(sqlite3_column_int64(q, i));
        else if (type == SQLITE_FLOAT)
            v = json_object_new_double(sqlite3_column_double(q, i));
        else if (type != SQLITE_NULL) {
            const char *str = (const char *)sqlite3_column_text(q, i);
            if (!strcmp(key, "rule") || !strcmp(key, "snapshot") || !strcmp(key, "data") ||
                !strcmp(key, "context") || !strcmp(key, "output"))
                v = json_tokener_parse(str);
            else
                v = json_object_new_string(str);
        }
        json_object_object_add(o, key, v);
    }
    return o;
}
static int exec_locked(Store *s, const char *sql)
{
    return sqlite3_exec(s->db, sql, NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
}
/* The definition upgrade and old-run retirement commit together. History is
 * deliberately never interpreted or rewritten. */
static int migrate_v3(Store *s, char *e, size_t n)
{
    sqlite3_stmt *q = prepare(s, "SELECT id,version,rule_json FROM spiders ORDER BY id");
    json_object *rows = json_object_new_array();
    if (!q) {
        json_object_put(rows);
        return -1;
    }
    int result;
    while ((result = sqlite3_step(q)) == SQLITE_ROW)
        json_object_array_add(rows, row_json(q));
    sqlite3_finalize(q);
    if (result != SQLITE_DONE) {
        json_object_put(rows);
        return -1;
    }
    int rc = 0;
    for (size_t i = 0; !rc && i < json_object_array_length(rows); i++) {
        json_object *entry = json_object_array_get_idx(rows, i);
        int64_t spider = json_object_get_int64(jg(entry, "id"));
        char reason[SP_ERROR] = "";
        json_object *old = json_parse(js(entry, "rule_json", ""), reason, sizeof(reason));
        if (!old) {
            rc = fail(e, n, "SQLite v3 migration: spider %lld has invalid rule JSON: %s",
                      (long long)spider, reason);
            break;
        }
        if (ji(old, "schema_version", 1) == 2) {
            json_object_put(old);
            continue;
        }
        json_object *upgraded = rule_upgrade_v1(old, reason, sizeof(reason));
        json_object_put(old);
        if (!upgraded) {
            rc = fail(e, n, "SQLite v3 migration: cannot upgrade spider %lld: %s",
                      (long long)spider, reason);
            break;
        }
        int64_t version = json_object_get_int64(jg(entry, "version"));
        if (version == INT64_MAX) {
            json_object_put(upgraded);
            rc = fail(e, n, "SQLite v3 migration: spider version overflow");
            break;
        }
        version++;
        int64_t timestamp = now_ms();
        const char *raw = json_object_to_json_string_ext(upgraded, JSON_C_TO_STRING_PLAIN);
        q = prepare(s, "UPDATE spiders SET rule_json=?1,version=?2,updated_at=?3 WHERE id=?4");
        if (!q) {
            json_object_put(upgraded);
            rc = -1;
            break;
        }
        txt(q, 1, raw);
        sqlite3_bind_int64(q, 2, version);
        sqlite3_bind_int64(q, 3, timestamp);
        sqlite3_bind_int64(q, 4, spider);
        rc = sqlite3_step(q) == SQLITE_DONE ? 0 : -1;
        sqlite3_finalize(q);
        q = NULL;
        if (!rc) {
            q = prepare(s, "INSERT INTO rule_versions(spider_id,version,rule_json,created_at) "
                           "VALUES(?,?,?,?)");
            if (!q)
                rc = -1;
            else {
                sqlite3_bind_int64(q, 1, spider);
                sqlite3_bind_int64(q, 2, version);
                txt(q, 3, raw);
                sqlite3_bind_int64(q, 4, timestamp);
                rc = sqlite3_step(q) == SQLITE_DONE ? 0 : -1;
            }
            sqlite3_finalize(q);
            q = NULL;
        }
        json_object_put(upgraded);
    }
    json_object_put(rows);
    if (!rc)
        rc = exec_locked(
            s,
            "UPDATE runs SET status='failed',error='SQLite v3 migration: unfinished legacy run was "
            "not replayed',"
            "finished_at=strftime('%s','now')*1000 WHERE status IN('queued','running') "
            "AND coalesce(json_extract(snapshot,'$.schema_version'),1)<>2;PRAGMA user_version=3;");
    return rc;
}
int store_open(Store *s, const char *path, char *e, size_t n)
{
    memset(s, 0, sizeof(*s));
    pthread_mutex_init(&s->mutex, NULL);
    if (sqlite3_open_v2(path, &s->db,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        NULL) != SQLITE_OK) {
        fail(e, n, "Cannot open SQLite: %s", sqlite3_errmsg(s->db));
        store_close(s);
        return -1;
    }
    sqlite3_busy_timeout(s->db, 5000);
    sqlite3_stmt *q = prepare(s, "PRAGMA user_version");
    int version = 0;
    if (q && sqlite3_step(q) == SQLITE_ROW)
        version = sqlite3_column_int(q, 0);
    sqlite3_finalize(q);
    if (e && n)
        *e = 0;
    if (version > 3) {
        fail(e, n, "Unsupported SQLite schema version: %d", version);
        store_close(s);
        return -1;
    }
    /* foreign_keys must be disabled before BEGIN for the v1 table rebuild.
     * Only this unopened Store can access the connection during migration. */
    if (exec_locked(s,
                    "PRAGMA foreign_keys=OFF;PRAGMA journal_mode=WAL;PRAGMA synchronous=FULL;") ||
        exec_locked(s, "BEGIN IMMEDIATE") || exec_locked(s, schema) ||
        (version < 2 && exec_locked(s, migration_v2)) || (version < 3 && migrate_v3(s, e, n)))
        goto failed;
    if (exec_locked(s, "UPDATE runs SET status='cancelled',finished_at=strftime('%s','now')*1000 "
                       "WHERE status IN('queued','running') AND cancel_requested=1;"
                       "UPDATE runs SET status='queued' WHERE status='running';"
                       "UPDATE step_instances SET status='queued' WHERE status='running' "
                       "AND run_id IN(SELECT id FROM runs WHERE status='queued');"
                       "UPDATE step_instances SET status='cancelled' WHERE status "
                       "IN('queued','running','waiting') "
                       "AND run_id IN(SELECT id FROM runs WHERE status IN('cancelled','failed'));"
                       "DELETE FROM sessions WHERE run_id IN(SELECT id FROM runs "
                       "WHERE status IN('succeeded','partial','failed','cancelled'));"))
        goto failed;
    if (exec_locked(s, "COMMIT") || exec_locked(s, "PRAGMA foreign_keys=ON"))
        goto failed;
    return 0;
failed:
    if (!e || !n || !*e)
        fail(e, n, "SQLite migration/recovery failed: %s", sqlite3_errmsg(s->db));
    exec_locked(s, "ROLLBACK");
    store_close(s);
    return -1;
}
void store_close(Store *s)
{
    if (s->db)
        sqlite3_close(s->db);
    s->db = NULL;
    pthread_mutex_destroy(&s->mutex);
}
json_object *store_query(Store *s, const char *sql, int64_t id, char *e, size_t n)
{
    pthread_mutex_lock(&s->mutex);
    sqlite3_stmt *q = prepare(s, sql);
    json_object *rows = NULL;
    if (q) {
        sqlite3_bind_int64(q, 1, id);
        rows = json_object_new_array();
        int rc;
        while ((rc = sqlite3_step(q)) == SQLITE_ROW)
            json_object_array_add(rows, row_json(q));
        if (rc != SQLITE_DONE) {
            json_object_put(rows);
            rows = NULL;
        }
    }
    if (!rows)
        fail(e, n, "Database query failed: %s", sqlite3_errmsg(s->db));
    sqlite3_finalize(q);
    pthread_mutex_unlock(&s->mutex);
    return rows;
}
int64_t store_save_spider(Store *s, int64_t id, json_object *r, char *e, size_t n)
{
    pthread_mutex_lock(&s->mutex);
    sqlite3_stmt *q = NULL;
    int64_t result = -1, version = 1;
    const char *raw = json_object_to_json_string_ext(r, JSON_C_TO_STRING_PLAIN);
    int interval = ji(r, "interval_seconds", 0);
    int64_t now = now_ms();
    if (exec_locked(s, "BEGIN IMMEDIATE"))
        goto done;
    if (id) {
        q = prepare(s, "SELECT version FROM spiders WHERE id=? AND deleted=0");
        if (!q)
            goto rollback;
        sqlite3_bind_int64(q, 1, id);
        if (sqlite3_step(q) != SQLITE_ROW) {
            fail(e, n, "Spider not found");
            goto rollback;
        }
        version = sqlite3_column_int64(q, 0) + 1;
        sqlite3_finalize(q);
        q = NULL;
    }
    q = prepare(s, id ? "UPDATE spiders SET "
                        "name=?1,rule_json=?2,enabled=?3,interval_seconds=?4,next_run=?5,updated_"
                        "at=?6,version=?7 WHERE id=?8"
                      : "INSERT INTO "
                        "spiders(name,rule_json,enabled,interval_seconds,next_run,updated_at,"
                        "version) VALUES(?1,?2,?3,?4,?5,?6,?7)");
    if (!q)
        goto rollback;
    txt(q, 1, js(r, "name", ""));
    txt(q, 2, raw);
    sqlite3_bind_int(q, 3, jb(r, "enabled", true));
    sqlite3_bind_int(q, 4, interval);
    sqlite3_bind_int64(q, 5, interval ? now + (int64_t)interval * 1000 : 0);
    sqlite3_bind_int64(q, 6, now);
    sqlite3_bind_int64(q, 7, version);
    if (id)
        sqlite3_bind_int64(q, 8, id);
    if (sqlite3_step(q) != SQLITE_DONE)
        goto rollback;
    sqlite3_finalize(q);
    q = NULL;
    if (!id)
        id = sqlite3_last_insert_rowid(s->db);
    q = prepare(s, "INSERT INTO rule_versions VALUES(?,?,?,?)");
    if (!q)
        goto rollback;
    sqlite3_bind_int64(q, 1, id);
    sqlite3_bind_int64(q, 2, version);
    txt(q, 3, raw);
    sqlite3_bind_int64(q, 4, now);
    if (sqlite3_step(q) != SQLITE_DONE)
        goto rollback;
    sqlite3_finalize(q);
    q = NULL;
    if (exec_locked(s, "COMMIT"))
        goto rollback;
    result = id;
    goto done;
rollback:
    sqlite3_finalize(q);
    q = NULL;
    if (!e || !*e)
        fail(e, n, "Cannot save rule: %s", sqlite3_errmsg(s->db));
    exec_locked(s, "ROLLBACK");
done:
    sqlite3_finalize(q);
    pthread_mutex_unlock(&s->mutex);
    return result;
}
int store_delete_spider(Store *s, int64_t id, char *e, size_t n)
{
    pthread_mutex_lock(&s->mutex);
    sqlite3_stmt *q = prepare(
        s, "UPDATE spiders SET deleted=1,enabled=0 WHERE id=? AND deleted=0 AND NOT EXISTS(SELECT "
           "1 FROM runs WHERE spider_id=spiders.id AND status IN('queued','running'))");
    int rc = -1;
    if (q) {
        sqlite3_bind_int64(q, 1, id);
        if (sqlite3_step(q) == SQLITE_DONE && sqlite3_changes(s->db))
            rc = 0;
    }
    if (rc)
        fail(e, n, "Spider not found or still running");
    sqlite3_finalize(q);
    pthread_mutex_unlock(&s->mutex);
    return rc;
}
static int64_t start_locked(Store *s, int64_t id)
{
    sqlite3_stmt *q = prepare(s, "INSERT INTO runs(spider_id,version,snapshot,created_at) SELECT "
                                 "id,version,rule_json,?2 FROM spiders WHERE id=?1 AND deleted=0");
    if (!q)
        return -1;
    sqlite3_bind_int64(q, 1, id);
    sqlite3_bind_int64(q, 2, now_ms());
    int rc = sqlite3_step(q);
    int changed = sqlite3_changes(s->db);
    sqlite3_finalize(q);
    return rc == SQLITE_DONE && changed ? sqlite3_last_insert_rowid(s->db) : -1;
}
int64_t store_start_run(Store *s, int64_t id, char *e, size_t n)
{
    pthread_mutex_lock(&s->mutex);
    int64_t run = start_locked(s, id);
    if (run < 0)
        fail(e, n, "Spider not found or already has an active run");
    pthread_mutex_unlock(&s->mutex);
    return run;
}
int store_cancel_run(Store *s, int64_t run)
{
    pthread_mutex_lock(&s->mutex);
    sqlite3_stmt *q = NULL;
    int rc = -1;
    if (exec_locked(s, "BEGIN IMMEDIATE"))
        goto done;
    q = prepare(s, "UPDATE runs SET cancel_requested=1,status='cancelled',finished_at=?2 "
                   "WHERE id=?1 AND status IN('queued','running')");
    if (!q)
        goto rollback;
    sqlite3_bind_int64(q, 1, run);
    sqlite3_bind_int64(q, 2, now_ms());
    if (sqlite3_step(q) != SQLITE_DONE || !sqlite3_changes(s->db))
        goto rollback;
    sqlite3_finalize(q);
    q = prepare(s, "UPDATE step_instances SET status='cancelled' WHERE run_id=?1 "
                   "AND status IN('queued','running','waiting') AND EXISTS(SELECT 1 FROM runs "
                   "WHERE id=?1 AND status='cancelled')");
    if (!q)
        goto rollback;
    sqlite3_bind_int64(q, 1, run);
    if (sqlite3_step(q) != SQLITE_DONE)
        goto rollback;
    sqlite3_finalize(q);
    q = NULL;
    if (exec_locked(s, "COMMIT"))
        goto rollback;
    rc = 0;
    goto done;
rollback:
    exec_locked(s, "ROLLBACK");
done:
    sqlite3_finalize(q);
    pthread_mutex_unlock(&s->mutex);
    return rc;
}
int64_t store_add_artifact(Store *s, int64_t run, const char *url, const char *path,
                           const char *hash, int64_t bytes)
{
    pthread_mutex_lock(&s->mutex);
    sqlite3_stmt *q =
        prepare(s, "INSERT INTO artifacts(run_id,url,path,sha256,bytes) SELECT ?1,?2,?3,?4,?5 "
                   "WHERE EXISTS(SELECT 1 FROM runs WHERE id=?1 AND "
                   "status='running' AND cancel_requested=0) ON "
                   "CONFLICT(run_id,url) DO UPDATE SET "
                   "path=excluded.path,sha256=excluded.sha256,bytes=excluded.bytes RETURNING id");
    int64_t id = -1;
    if (q) {
        sqlite3_bind_int64(q, 1, run);
        txt(q, 2, url);
        txt(q, 3, path);
        txt(q, 4, hash);
        sqlite3_bind_int64(q, 5, bytes);
        if (sqlite3_step(q) == SQLITE_ROW)
            id = sqlite3_column_int64(q, 0);
    }
    sqlite3_finalize(q);
    pthread_mutex_unlock(&s->mutex);
    return id;
}
