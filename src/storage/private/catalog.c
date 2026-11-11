#include "storage_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
static sqlite3_stmt *prepare(Store *s, const char *sql)
{
    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(s->db, sql, -1, &q, NULL) != SQLITE_OK)
        return NULL;
    return q;
}
static void text(sqlite3_stmt *q, int index, const char *value)
{
    sqlite3_bind_text(q, index, value, -1, SQLITE_TRANSIENT);
}
int store_sync_code(Store *s, json_object *tasks, char *e, size_t n)
{
    if (e && n)
        *e = 0;
    if (!s || !json_object_is_type(tasks, json_type_array))
        return fail(e, n, "Invalid code crawler catalog");
    pthread_mutex_lock(&s->mutex);
    sqlite3_stmt *q = NULL;
    int rc = -1;
    if (sqlite3_exec(s->db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
        goto done;
    if (sqlite3_exec(s->db, "UPDATE code_spiders SET active=0", NULL, NULL, NULL) != SQLITE_OK)
        goto rollback;
    for (size_t i = 0; i < json_object_array_length(tasks); i++) {
        json_object *task = json_object_array_get_idx(tasks, i), *config = jg(task, "config");
        const char *key = js(task, "code_id", "");
        if (!*key || !json_object_is_type(config, json_type_object) || !*js(config, "name", "")) {
            fail(e, n, "Invalid code crawler entry");
            goto rollback;
        }
        for (size_t j = 0; j < i; j++)
            if (!strcmp(key, js(json_object_array_get_idx(tasks, j), "code_id", ""))) {
                fail(e, n, "Duplicate code crawler ID");
                goto rollback;
            }
        int64_t id = 0, version = 1;
        bool changed = true;
        q = prepare(s, "SELECT s.id,s.version,s.rule_json,s.deleted FROM code_spiders c JOIN "
                       "spiders s ON s.id=c.spider_id WHERE c.code_id=?");
        if (!q)
            goto rollback;
        text(q, 1, key);
        int result = sqlite3_step(q);
        if (result == SQLITE_ROW) {
            id = sqlite3_column_int64(q, 0);
            version = sqlite3_column_int64(q, 1);
            json_object *old = json_tokener_parse((const char *)sqlite3_column_text(q, 2));
            changed = !old || !json_object_equal(old, config) || sqlite3_column_int(q, 3) != 0;
            json_object_put(old);
            if (changed) {
                if (version == INT64_MAX) {
                    fail(e, n, "Crawler version overflow");
                    goto rollback;
                }
                version++;
            }
        } else if (result != SQLITE_DONE)
            goto rollback;
        sqlite3_finalize(q);
        q = NULL;
        if (changed) {
            const char *raw = json_object_to_json_string_ext(config, JSON_C_TO_STRING_PLAIN);
            int interval = ji(config, "interval_seconds", 0);
            int64_t now = now_ms();
            q = prepare(s, id ? "UPDATE spiders SET "
                                "name=?1,rule_json=?2,enabled=?3,interval_seconds=?4,next_run=?5,"
                                "updated_at=?6,version=?7,deleted=0 WHERE id=?8"
                              : "INSERT INTO "
                                "spiders(name,rule_json,enabled,interval_seconds,next_run,updated_"
                                "at,version) VALUES(?1,?2,?3,?4,?5,?6,?7)");
            if (!q)
                goto rollback;
            text(q, 1, js(config, "name", ""));
            text(q, 2, raw);
            sqlite3_bind_int(q, 3, jb(config, "enabled", true));
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
            q = prepare(s, "INSERT INTO rule_versions(spider_id,version,rule_json,created_at) "
                           "VALUES(?,?,?,?)");
            if (!q)
                goto rollback;
            sqlite3_bind_int64(q, 1, id);
            sqlite3_bind_int64(q, 2, version);
            text(q, 3, raw);
            sqlite3_bind_int64(q, 4, now);
            if (sqlite3_step(q) != SQLITE_DONE)
                goto rollback;
            sqlite3_finalize(q);
            q = NULL;
        }
        if (!changed) {
            /* Re-adding unchanged code restores its enabled state without
             * changing its identity, version or saved next_run. */
            q = prepare(s, "UPDATE spiders SET enabled=?2 WHERE id=?1");
            if (!q)
                goto rollback;
            sqlite3_bind_int64(q, 1, id);
            sqlite3_bind_int(q, 2, jb(config, "enabled", true));
            if (sqlite3_step(q) != SQLITE_DONE)
                goto rollback;
            sqlite3_finalize(q);
            q = NULL;
        }
        q = prepare(s, "INSERT INTO code_spiders(code_id,spider_id,active) VALUES(?,?,1) ON "
                       "CONFLICT(code_id) DO UPDATE SET active=1");
        if (!q)
            goto rollback;
        text(q, 1, key);
        sqlite3_bind_int64(q, 2, id);
        if (sqlite3_step(q) != SQLITE_DONE)
            goto rollback;
        sqlite3_finalize(q);
        q = NULL;
    }
    if (sqlite3_exec(s->db,
                     "UPDATE spiders SET enabled=0 WHERE id NOT IN(SELECT spider_id FROM "
                     "code_spiders WHERE active=1);"
                     "UPDATE runs SET status='cancelled',cancel_requested=1,error='Crawler not "
                     "registered by current code',"
                     "finished_at=strftime('%s','now')*1000 WHERE status IN('queued','running') "
                     "AND spider_id NOT IN(SELECT spider_id FROM code_spiders WHERE active=1);"
                     "UPDATE step_instances SET status='cancelled' WHERE status "
                     "IN('queued','running','waiting') "
                     "AND run_id IN(SELECT id FROM runs WHERE status='cancelled');"
                     "DELETE FROM sessions WHERE run_id IN(SELECT id FROM runs WHERE status "
                     "IN('cancelled','failed','succeeded','partial'));",
                     NULL, NULL, NULL) != SQLITE_OK)
        goto rollback;
    if (sqlite3_exec(s->db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK)
        goto rollback;
    rc = 0;
    goto done;
rollback:
    if (!e || !n || !*e)
        fail(e, n, "Cannot synchronize code crawlers: %s", sqlite3_errmsg(s->db));
    sqlite3_finalize(q);
    q = NULL;
    sqlite3_exec(s->db, "ROLLBACK", NULL, NULL, NULL);
done:
    if (rc && (!e || !n || !*e))
        fail(e, n, "Cannot synchronize code crawlers: %s", sqlite3_errmsg(s->db));
    sqlite3_finalize(q);
    pthread_mutex_unlock(&s->mutex);
    return rc;
}
static const char *catalog_query =
    "SELECT s.id,c.code_id,s.name,s.enabled,s.interval_seconds,s.next_run,s.version,"
    "r.id AS latest_run_id,coalesce(r.status,'idle') AS "
    "status,r.created_at,r.started_at,r.finished_at,"
    "coalesce(r.error,'') AS error,coalesce(r.pages,0) AS pages,coalesce(r.item_count,0) AS "
    "item_count,"
    "(SELECT count(*) FROM runs x WHERE x.spider_id=s.id AND x.status='succeeded') AS successes,"
    "(SELECT count(*) FROM runs x WHERE x.spider_id=s.id AND x.status='failed') AS failures "
    "FROM code_spiders c JOIN spiders s ON s.id=c.spider_id LEFT JOIN runs r ON "
    "r.id=(SELECT max(x.id) FROM runs x WHERE x.spider_id=s.id) WHERE c.active=1 AND s.deleted=0 ";
static json_object *catalog(Store *s, int64_t id, char *e, size_t n)
{
    char *sql = sqlite3_mprintf("%s AND (?1=0 OR s.id=?1) ORDER BY c.code_id", catalog_query);
    if (!sql) {
        fail(e, n, "Cannot allocate catalog query");
        return NULL;
    }
    json_object *rows = store_query(s, sql, id, e, n);
    sqlite3_free(sql);
    return rows;
}
json_object *store_code_spiders(Store *s, char *e, size_t n)
{
    return catalog(s, 0, e, n);
}
json_object *store_code_spider(Store *s, int64_t id)
{
    json_object *rows = catalog(s, id, NULL, 0), *row = NULL;
    if (rows && json_object_array_length(rows))
        row = json_object_get(json_object_array_get_idx(rows, 0));
    json_object_put(rows);
    return row;
}
json_object *store_code_runs(Store *s, int64_t spider, char *e, size_t n)
{
    return store_query(s,
                       "SELECT r.id,r.spider_id,r.version,r.status,r.pages,r.item_count,r.error,"
                       "r.created_at,r.started_at,r.finished_at FROM runs r JOIN code_spiders c ON "
                       "c.spider_id=r.spider_id "
                       "WHERE c.active=1 AND (?1=0 OR r.spider_id=?1) ORDER BY r.id DESC LIMIT 200",
                       spider, e, n);
}
bool store_code_has_run(Store *s, int64_t run)
{
    json_object *rows = store_query(s,
                                    "SELECT r.id FROM runs r JOIN code_spiders c ON "
                                    "c.spider_id=r.spider_id WHERE r.id=? AND c.active=1",
                                    run, NULL, 0);
    bool found = rows && json_object_array_length(rows) > 0;
    json_object_put(rows);
    return found;
}
