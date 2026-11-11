#include "storage/private/storage_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

Store *store_create(const char *path, char *e, size_t n)
{
    Store *s = calloc(1, sizeof(*s));
    if (!s) {
        fail(e, n, "Cannot allocate storage");
        return NULL;
    }
    if (store_open(s, path, e, n)) {
        free(s);
        return NULL;
    }
    return s;
}
void store_destroy(Store *s)
{
    if (s) {
        store_close(s);
        free(s);
    }
}
static json_object *one(Store *s, const char *sql, int64_t id)
{
    json_object *rows = store_query(s, sql, id, NULL, 0);
    json_object *value = rows && json_object_array_length(rows)
                             ? json_object_get(json_object_array_get_idx(rows, 0))
                             : NULL;
    json_object_put(rows);
    return value;
}
json_object *store_spiders(Store *s, char *e, size_t n)
{
    return store_query(s,
                       "SELECT id,name,version,enabled,interval_seconds,next_run,updated_at "
                       "FROM spiders WHERE deleted=0 ORDER BY id DESC",
                       0, e, n);
}
json_object *store_spider(Store *s, int64_t id)
{
    return one(s, "SELECT id,name,version,rule_json AS rule FROM spiders WHERE id=? AND deleted=0",
               id);
}
json_object *store_runs(Store *s, int64_t spider, char *e, size_t n)
{
    return store_query(
        s,
        "SELECT id,spider_id,version,status,pages,item_count,error,created_at,"
        "started_at,finished_at FROM runs WHERE (?1=0 OR spider_id=?1) ORDER BY id DESC LIMIT 200",
        spider, e, n);
}
json_object *store_run(Store *s, int64_t id)
{
    return one(s, "SELECT * FROM runs WHERE id=?", id);
}
json_object *store_items(Store *s, int64_t run, int64_t after, char *e, size_t n)
{
    char sql[256];
    snprintf(sql, sizeof(sql),
             "SELECT id,source_url,data,created_at FROM items WHERE run_id=? "
             "AND id>%lld ORDER BY id LIMIT 1000",
             (long long)after);
    return store_query(s, sql, run, e, n);
}
json_object *store_events(Store *s, int64_t run, char *e, size_t n)
{
    return store_query(s,
                       "SELECT id,level,message,created_at FROM run_events WHERE run_id=? "
                       "ORDER BY id DESC LIMIT 200",
                       run, e, n);
}
json_object *store_steps(Store *s, int64_t run, char *e, size_t n)
{
    return store_query(s,
                       "SELECT id,run_id,step_id,parent_id,branch_key,record_key,status,attempts,"
                       "error,phase FROM step_instances WHERE run_id=? ORDER BY id DESC LIMIT 1000",
                       run, e, n);
}
json_object *store_step(Store *s, int64_t run, int64_t instance)
{
    char sql[320];
    snprintf(sql, sizeof(sql),
             "SELECT id,run_id,step_id,parent_id,branch_key,record_key,status,attempts,"
             "context,output,error,phase FROM step_instances WHERE run_id=? AND id=%lld",
             (long long)instance);
    return one(s, sql, run);
}
json_object *store_artifacts(Store *s, int64_t run, char *e, size_t n)
{
    return store_query(
        s, "SELECT id,url,path,sha256,bytes FROM artifacts WHERE run_id=? ORDER BY id", run, e, n);
}
json_object *store_artifact(Store *s, int64_t id)
{
    return one(s, "SELECT path FROM artifacts WHERE id=?", id);
}
json_object *store_due(Store *s, int64_t now, char *e, size_t n)
{
    return store_query(s,
                       "SELECT id,interval_seconds FROM spiders WHERE deleted=0 AND enabled=1 "
                       "AND interval_seconds>0 AND next_run<=? ORDER BY id",
                       now, e, n);
}
int store_next_due(Store *s, int64_t id, int64_t next)
{
    pthread_mutex_lock(&s->mutex);
    sqlite3_stmt *q = NULL;
    int rc = sqlite3_prepare_v2(s->db, "UPDATE spiders SET next_run=?2 WHERE id=?1", -1, &q, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int64(q, 1, id);
        sqlite3_bind_int64(q, 2, next);
        rc = sqlite3_step(q);
    }
    sqlite3_finalize(q);
    pthread_mutex_unlock(&s->mutex);
    return rc == SQLITE_DONE ? 0 : -1;
}
json_object *store_claim_run(Store *s, const int64_t *blocked, size_t count, char *e, size_t n)
{
    if (e && n)
        *e = 0;
    pthread_mutex_lock(&s->mutex);
    json_object *result = NULL;
    sqlite3_stmt *q = NULL;
    if (sqlite3_exec(s->db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
        goto done;
    if (sqlite3_prepare_v2(s->db,
                           "SELECT id,spider_id,snapshot FROM runs WHERE status='queued' "
                           "AND cancel_requested=0 ORDER BY id",
                           -1, &q, NULL) != SQLITE_OK)
        goto rollback;
    int code;
    while ((code = sqlite3_step(q)) == SQLITE_ROW) {
        int64_t spider = sqlite3_column_int64(q, 1);
        bool busy = false;
        for (size_t i = 0; i < count; i++)
            if (blocked[i] == spider)
                busy = true;
        if (busy)
            continue;
        result = json_object_new_object();
        json_object_object_add(result, "id", json_object_new_int64(sqlite3_column_int64(q, 0)));
        json_object_object_add(result, "spider_id", json_object_new_int64(spider));
        json_object_object_add(result, "snapshot",
                               json_tokener_parse((const char *)sqlite3_column_text(q, 2)));
        break;
    }
    if (code != SQLITE_ROW && code != SQLITE_DONE)
        goto rollback;
    sqlite3_finalize(q);
    q = NULL;
    if (result) {
        if (sqlite3_prepare_v2(
                s->db,
                "UPDATE runs SET status='running',started_at=coalesce(started_at,?2) "
                "WHERE id=?1 AND status='queued'",
                -1, &q, NULL) != SQLITE_OK)
            goto rollback;
        sqlite3_bind_int64(q, 1, json_object_get_int64(jg(result, "id")));
        sqlite3_bind_int64(q, 2, now_ms());
        if (sqlite3_step(q) != SQLITE_DONE || sqlite3_changes(s->db) != 1)
            goto rollback;
        sqlite3_finalize(q);
        q = NULL;
    }
    if (sqlite3_exec(s->db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK)
        goto rollback;
    goto done;
rollback:
    fail(e, n, "Cannot claim crawler run: %s", sqlite3_errmsg(s->db));
    sqlite3_exec(s->db, "ROLLBACK", NULL, NULL, NULL);
    json_object_put(result);
    result = NULL;
done:
    sqlite3_finalize(q);
    pthread_mutex_unlock(&s->mutex);
    return result;
}
int store_finish_run(Store *s, int64_t run, const char *status, const char *error)
{
    pthread_mutex_lock(&s->mutex);
    sqlite3_stmt *q = NULL;
    int rc = sqlite3_prepare_v2(s->db,
                                "UPDATE runs SET status=?2,error=?3,finished_at=?4 "
                                "WHERE id=?1 AND status='running' AND cancel_requested=0",
                                -1, &q, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int64(q, 1, run);
        sqlite3_bind_text(q, 2, status, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(q, 3, error ? error : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(q, 4, now_ms());
        rc = sqlite3_step(q);
    }
    sqlite3_finalize(q);
    pthread_mutex_unlock(&s->mutex);
    return rc == SQLITE_DONE ? 0 : -1;
}
bool store_run_active(Store *s, int64_t run)
{
    json_object *row = store_run(s, run);
    bool result =
        row && !ji(row, "cancel_requested", 0) && !strcmp(js(row, "status", ""), "running");
    json_object_put(row);
    return result;
}
bool store_step_has_post(Store *s, int64_t step)
{
    json_object *row =
        one(s, "SELECT count(*) AS count FROM requests WHERE step_instance_id=? AND method='POST'",
            step);
    bool result = !row || ji(row, "count", 0) > 0;
    json_object_put(row);
    return result;
}
int64_t store_step_iteration(Store *s, int64_t step)
{
    json_object *row =
        one(s,
            "SELECT count(*) AS iteration FROM step_instances a JOIN step_instances b ON b.id=? "
            "WHERE a.run_id=b.run_id AND a.step_id=b.step_id AND a.record_key=b.record_key "
            "AND a.branch_key=b.branch_key AND a.id<=b.id",
            step);
    int64_t value = row ? json_object_get_int64(jg(row, "iteration")) : -1;
    json_object_put(row);
    return value;
}
json_object *store_session_load(Store *s, int64_t run, char *e, size_t n)
{
    json_object *rows = store_query(s, "SELECT cookies FROM sessions WHERE run_id=?", run, e, n);
    if (!rows)
        return NULL;
    json_object *cookies =
        json_object_array_length(rows)
            ? json_tokener_parse(js(json_object_array_get_idx(rows, 0), "cookies", "[]"))
            : json_object_new_array();
    json_object_put(rows);
    return cookies;
}
int store_session_save(Store *s, int64_t run, json_object *cookies, char *e, size_t n)
{
    const char *text = json_object_to_json_string_ext(cookies, JSON_C_TO_STRING_PLAIN);
    if (!text || strlen(text) > 2097152)
        return fail(e, n, "Cookie session exceeds storage limit");
    pthread_mutex_lock(&s->mutex);
    sqlite3_stmt *q = NULL;
    int rc = sqlite3_prepare_v2(
        s->db,
        "INSERT INTO sessions(run_id,cookies) SELECT ?1,?2 WHERE EXISTS(SELECT 1 FROM runs "
        "WHERE id=?1 AND status='running' AND cancel_requested=0) ON CONFLICT(run_id) DO UPDATE "
        "SET cookies=excluded.cookies",
        -1, &q, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int64(q, 1, run);
        sqlite3_bind_text(q, 2, text, -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(q);
    }
    sqlite3_finalize(q);
    pthread_mutex_unlock(&s->mutex);
    return rc == SQLITE_DONE ? 0 : fail(e, n, "Cannot persist Cookie session");
}
void store_session_prune(Store *s, int64_t run)
{
    pthread_mutex_lock(&s->mutex);
    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(
            s->db,
            "DELETE FROM sessions WHERE run_id=?1 AND EXISTS(SELECT 1 FROM runs WHERE "
            "id=?1 AND status NOT IN('queued','running'))",
            -1, &q, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(q, 1, run);
        sqlite3_step(q);
    }
    sqlite3_finalize(q);
    pthread_mutex_unlock(&s->mutex);
}
int store_emit(Store *s, int64_t run, const char *key, const char *source, json_object *value,
               char *e, size_t n)
{
    if (e && n)
        *e = 0;
    if (!key || !*key || !json_object_is_type(value, json_type_object))
        return fail(e, n, "Native items need a stable key and object value");
    pthread_mutex_lock(&s->mutex);
    sqlite3_stmt *q = NULL;
    int rc = -1;
    if (sqlite3_exec(s->db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) {
        fail(e, n, "Cannot start item transaction: %s", sqlite3_errmsg(s->db));
        goto done;
    }
    if (sqlite3_prepare_v2(
            s->db, "SELECT 1 FROM runs WHERE id=? AND status='running' AND cancel_requested=0", -1,
            &q, NULL) != SQLITE_OK)
        goto rollback;
    sqlite3_bind_int64(q, 1, run);
    if (sqlite3_step(q) != SQLITE_ROW) {
        fail(e, n, "Crawler run is no longer active");
        goto rollback;
    }
    sqlite3_finalize(q);
    q = NULL;
    if (sqlite3_prepare_v2(
            s->db,
            "INSERT INTO items(run_id,record_key,step_id,source_url,data,created_at) "
            "SELECT ?1,?2,'native',?3,?4,?5 WHERE EXISTS(SELECT 1 FROM runs WHERE id=?1 AND "
            "status='running' AND cancel_requested=0) "
            "AND NOT EXISTS(SELECT 1 FROM items WHERE run_id=?1 AND record_key=?2 AND "
            "step_id='native')",
            -1, &q, NULL) != SQLITE_OK)
        goto rollback;
    sqlite3_bind_int64(q, 1, run);
    sqlite3_bind_text(q, 2, key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(q, 3, source, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(q, 4, json_object_to_json_string_ext(value, JSON_C_TO_STRING_PLAIN), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_int64(q, 5, now_ms());
    if (sqlite3_step(q) != SQLITE_DONE)
        goto rollback;
    int added = sqlite3_changes(s->db);
    sqlite3_finalize(q);
    q = NULL;
    if (added) {
        if (sqlite3_prepare_v2(s->db, "UPDATE runs SET item_count=item_count+1 WHERE id=?", -1, &q,
                               NULL) != SQLITE_OK)
            goto rollback;
        sqlite3_bind_int64(q, 1, run);
        if (sqlite3_step(q) != SQLITE_DONE)
            goto rollback;
    }
    if (sqlite3_exec(s->db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK)
        goto rollback;
    rc = 0;
    goto done;
rollback:
    if (!e || !n || !*e)
        fail(e, n, "Cannot save crawler item: %s", sqlite3_errmsg(s->db));
    sqlite3_exec(s->db, "ROLLBACK", NULL, NULL, NULL);
done:
    sqlite3_finalize(q);
    pthread_mutex_unlock(&s->mutex);
    return rc;
}
int store_record_request(Store *s, json_object *job, json_object *request, const char *status,
                         char *e, size_t n)
{
    pthread_mutex_lock(&s->mutex);
    sqlite3_stmt *q = NULL;
    int rc = -1;
    const char *sql =
        "INSERT INTO requests(run_id,step_instance_id,url,method,request_key,status,attempts) "
        "SELECT ?1,?2,?3,?4,?5,?6,?7 WHERE EXISTS(SELECT 1 FROM runs WHERE id=?1 AND "
        "status='running' AND cancel_requested=0) ON CONFLICT(run_id,request_key) WHERE "
        "request_key IS NOT NULL DO UPDATE SET status=excluded.status,attempts=excluded.attempts";
    char key[100];
    snprintf(key, sizeof(key), "step:%lld:attempt:%d",
             (long long)json_object_get_int64(jg(job, "id")), ji(job, "attempts", 1));
    if (sqlite3_prepare_v2(s->db, sql, -1, &q, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(q, 1, json_object_get_int64(jg(job, "run_id")));
        sqlite3_bind_int64(q, 2, json_object_get_int64(jg(job, "id")));
        sqlite3_bind_text(q, 3, js(request, "url", ""), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(q, 4, js(request, "method", "GET"), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(q, 5, key, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(q, 6, status, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(q, 7, ji(job, "attempts", 1));
        if (sqlite3_step(q) == SQLITE_DONE && sqlite3_changes(s->db))
            rc = 0;
    }
    if (rc)
        fail(e, n, "Cannot persist workflow HTTP attempt: %s", sqlite3_errmsg(s->db));
    sqlite3_finalize(q);
    pthread_mutex_unlock(&s->mutex);
    return rc;
}
