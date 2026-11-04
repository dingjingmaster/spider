#include "storage.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static const char *schema =
    "PRAGMA foreign_keys=ON;PRAGMA journal_mode=WAL;PRAGMA synchronous=FULL;"
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
    "CREATE INDEX IF NOT EXISTS items_run ON items(run_id,id);"
    "PRAGMA user_version=1;";

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
            if (!strcmp(key, "rule") || !strcmp(key, "snapshot") || !strcmp(key, "data"))
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
    if (version > 1 || exec_locked(s, schema)) {
        fail(e, n, "Unsupported or invalid SQLite schema: %s", sqlite3_errmsg(s->db));
        store_close(s);
        return -1;
    }
    if (exec_locked(s, "BEGIN IMMEDIATE;UPDATE runs SET "
                       "status='cancelled',finished_at=strftime('%s','now')*1000 WHERE status "
                       "IN('queued','running') AND cancel_requested=1;UPDATE requests SET "
                       "status='queued' WHERE status='running';UPDATE runs SET status='queued' "
                       "WHERE status='running';COMMIT;")) {
        fail(e, n, "Database recovery failed");
        store_close(s);
        return -1;
    }
    return 0;
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
json_object *store_claim_run(Store *s)
{
    pthread_mutex_lock(&s->mutex);
    sqlite3_stmt *q = prepare(
        s,
        "UPDATE runs SET status='running',started_at=coalesce(started_at,?1) WHERE id=(SELECT id "
        "FROM runs WHERE status='queued' AND cancel_requested=0 ORDER BY id LIMIT 1) RETURNING *");
    json_object *out = NULL;
    if (q) {
        sqlite3_bind_int64(q, 1, now_ms());
        if (sqlite3_step(q) == SQLITE_ROW)
            out = row_json(q);
    }
    sqlite3_finalize(q);
    pthread_mutex_unlock(&s->mutex);
    return out;
}
int store_finish_run(Store *s, int64_t run, const char *status, const char *error)
{
    pthread_mutex_lock(&s->mutex);
    sqlite3_stmt *q = prepare(s, "UPDATE runs SET status=CASE WHEN cancel_requested=1 THEN "
                                 "'cancelled' ELSE ?2 END,error=?3,finished_at=?4 WHERE id=?1");
    int rc = -1;
    if (q) {
        sqlite3_bind_int64(q, 1, run);
        txt(q, 2, status);
        txt(q, 3, error);
        sqlite3_bind_int64(q, 4, now_ms());
        rc = sqlite3_step(q) == SQLITE_DONE ? 0 : -1;
    }
    sqlite3_finalize(q);
    pthread_mutex_unlock(&s->mutex);
    return rc;
}
int store_cancel_run(Store *s, int64_t run)
{
    pthread_mutex_lock(&s->mutex);
    sqlite3_stmt *q =
        prepare(s, "UPDATE runs SET cancel_requested=1,status=CASE WHEN status='queued' THEN "
                   "'cancelled' ELSE status END,finished_at=CASE WHEN status='queued' THEN ?2 ELSE "
                   "finished_at END WHERE id=?1 AND status IN('queued','running')");
    int rc = -1;
    if (q) {
        sqlite3_bind_int64(q, 1, run);
        sqlite3_bind_int64(q, 2, now_ms());
        if (sqlite3_step(q) == SQLITE_DONE && sqlite3_changes(s->db))
            rc = 0;
    }
    sqlite3_finalize(q);
    pthread_mutex_unlock(&s->mutex);
    return rc;
}
bool store_cancelled(Store *s, int64_t run)
{
    pthread_mutex_lock(&s->mutex);
    sqlite3_stmt *q = prepare(s, "SELECT cancel_requested FROM runs WHERE id=?");
    bool result = true;
    if (q) {
        sqlite3_bind_int64(q, 1, run);
        if (sqlite3_step(q) == SQLITE_ROW)
            result = sqlite3_column_int(q, 0) != 0;
    }
    sqlite3_finalize(q);
    pthread_mutex_unlock(&s->mutex);
    return result;
}
int store_event(Store *s, int64_t run, const char *level, const char *message)
{
    pthread_mutex_lock(&s->mutex);
    sqlite3_stmt *q =
        prepare(s, "INSERT INTO run_events(run_id,level,message,created_at) VALUES(?,?,?,?)");
    int rc = -1;
    if (q) {
        sqlite3_bind_int64(q, 1, run);
        txt(q, 2, level);
        txt(q, 3, message);
        sqlite3_bind_int64(q, 4, now_ms());
        rc = sqlite3_step(q) == SQLITE_DONE ? 0 : -1;
    }
    sqlite3_finalize(q);
    pthread_mutex_unlock(&s->mutex);
    return rc;
}
int64_t store_enqueue(Store *s, int64_t run, const char *url)
{
    pthread_mutex_lock(&s->mutex);
    sqlite3_stmt *q = prepare(s, "INSERT INTO requests(run_id,url) VALUES(?,?) ON "
                                 "CONFLICT(run_id,url) DO NOTHING RETURNING id");
    int64_t id = -1;
    if (q) {
        sqlite3_bind_int64(q, 1, run);
        txt(q, 2, url);
        int rc = sqlite3_step(q);
        if (rc == SQLITE_ROW)
            id = sqlite3_column_int64(q, 0);
        else if (rc == SQLITE_DONE)
            id = 0;
    }
    sqlite3_finalize(q);
    pthread_mutex_unlock(&s->mutex);
    return id;
}
json_object *store_claim_request(Store *s, int64_t run, char *error, size_t cap)
{
    pthread_mutex_lock(&s->mutex);
    sqlite3_stmt *q = prepare(
        s, "UPDATE requests SET status='running',attempts=attempts+1 WHERE id=(SELECT id FROM "
           "requests WHERE run_id=? AND status='queued' ORDER BY id LIMIT 1) RETURNING *");
    json_object *out = NULL;
    if (error && cap)
        error[0] = 0;
    if (q) {
        sqlite3_bind_int64(q, 1, run);
        int result = sqlite3_step(q);
        if (result == SQLITE_ROW)
            out = row_json(q);
        else if (result != SQLITE_DONE)
            fail(error, cap, "Cannot claim request: %s", sqlite3_errmsg(s->db));
    } else {
        fail(error, cap, "Cannot prepare request claim: %s", sqlite3_errmsg(s->db));
    }
    sqlite3_finalize(q);
    pthread_mutex_unlock(&s->mutex);
    return out;
}
int store_request_status(Store *s, int64_t request, const char *status, const char *error)
{
    pthread_mutex_lock(&s->mutex);
    sqlite3_stmt *q = prepare(s, "UPDATE requests SET status=?,error=? WHERE id=?");
    int rc = -1;
    if (q) {
        txt(q, 1, status);
        txt(q, 2, error);
        sqlite3_bind_int64(q, 3, request);
        rc = sqlite3_step(q) == SQLITE_DONE ? 0 : -1;
    }
    sqlite3_finalize(q);
    pthread_mutex_unlock(&s->mutex);
    return rc;
}
int store_commit_page(Store *s, int64_t run, int64_t req, const char *url, json_object *items,
                      const char *next)
{
    pthread_mutex_lock(&s->mutex);
    sqlite3_stmt *q = NULL;
    int rc = -1, added = 0;
    if (exec_locked(s, "BEGIN IMMEDIATE"))
        goto done;
    q = prepare(s, "INSERT INTO items(run_id,request_id,source_url,data,created_at) "
                   "VALUES(?,?,?,?,?) ON CONFLICT(request_id,data) DO NOTHING");
    if (!q)
        goto rollback;
    for (size_t i = 0; i < json_object_array_length(items); i++) {
        sqlite3_reset(q);
        sqlite3_bind_int64(q, 1, run);
        sqlite3_bind_int64(q, 2, req);
        txt(q, 3, url);
        txt(q, 4,
            json_object_to_json_string_ext(json_object_array_get_idx(items, i),
                                           JSON_C_TO_STRING_PLAIN));
        sqlite3_bind_int64(q, 5, now_ms());
        if (sqlite3_step(q) != SQLITE_DONE)
            goto rollback;
        added += sqlite3_changes(s->db);
    }
    sqlite3_finalize(q);
    q = prepare(s, "UPDATE requests SET status='succeeded',error='' WHERE id=?");
    if (!q)
        goto rollback;
    sqlite3_bind_int64(q, 1, req);
    if (sqlite3_step(q) != SQLITE_DONE)
        goto rollback;
    sqlite3_finalize(q);
    q = prepare(s, "UPDATE runs SET pages=pages+1,item_count=item_count+?2 WHERE id=?1");
    if (!q)
        goto rollback;
    sqlite3_bind_int64(q, 1, run);
    sqlite3_bind_int(q, 2, added);
    if (sqlite3_step(q) != SQLITE_DONE)
        goto rollback;
    sqlite3_finalize(q);
    q = NULL;
    if (next) {
        q = prepare(
            s, "INSERT INTO requests(run_id,url) VALUES(?,?) ON CONFLICT(run_id,url) DO NOTHING");
        if (!q)
            goto rollback;
        sqlite3_bind_int64(q, 1, run);
        txt(q, 2, next);
        if (sqlite3_step(q) != SQLITE_DONE)
            goto rollback;
        sqlite3_finalize(q);
        q = NULL;
    }
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
        prepare(s, "INSERT INTO artifacts(run_id,url,path,sha256,bytes) VALUES(?,?,?,?,?) ON "
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
void store_schedule(Store *s)
{
    pthread_mutex_lock(&s->mutex);
    sqlite3_stmt *q = prepare(s, "SELECT id,interval_seconds FROM spiders WHERE deleted=0 AND "
                                 "enabled=1 AND interval_seconds>0 AND next_run<=?");
    json_object *due = json_object_new_array();
    if (q) {
        sqlite3_bind_int64(q, 1, now_ms());
        while (sqlite3_step(q) == SQLITE_ROW)
            json_object_array_add(due, row_json(q));
    }
    sqlite3_finalize(q);
    for (size_t i = 0; i < json_object_array_length(due); i++) {
        json_object *row = json_object_array_get_idx(due, i);
        int64_t id = json_object_get_int64(jg(row, "id"));
        start_locked(s, id);
        q = prepare(s, "UPDATE spiders SET next_run=? WHERE id=?");
        if (q) {
            sqlite3_bind_int64(q, 1, now_ms() + (int64_t)ji(row, "interval_seconds", 1) * 1000);
            sqlite3_bind_int64(q, 2, id);
            sqlite3_step(q);
        }
        sqlite3_finalize(q);
    }
    json_object_put(due);
    pthread_mutex_unlock(&s->mutex);
}
