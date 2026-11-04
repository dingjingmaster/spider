#include "workflow.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WF_CONTEXT_LIMIT (4U * 1024U * 1024U)
#define WF_FANOUT_LIMIT 1000U

static int sql(Store *s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *query = sqlite3_vmprintf(fmt, ap);
    va_end(ap);
    if (!query)
        return -1;
    int rc = sqlite3_exec(s->db, query, NULL, NULL, NULL);
    sqlite3_free(query);
    return rc == SQLITE_OK ? 0 : -1;
}
static sqlite3_stmt *prepare(Store *s, const char *query)
{
    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(s->db, query, -1, &q, NULL) != SQLITE_OK)
        return NULL;
    return q;
}
static int64_t id(json_object *o, const char *key)
{
    return json_object_get_int64(jg(o, key));
}
static const char *raw(json_object *o)
{
    return o ? json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN) : "null";
}
static json_object *row(sqlite3_stmt *q)
{
    json_object *o = json_object_new_object();
    for (int i = 0; i < sqlite3_column_count(q); i++) {
        const char *key = sqlite3_column_name(q, i);
        json_object *v = NULL;
        if (sqlite3_column_type(q, i) == SQLITE_INTEGER)
            v = json_object_new_int64(sqlite3_column_int64(q, i));
        else if (sqlite3_column_type(q, i) != SQLITE_NULL) {
            const char *value = (const char *)sqlite3_column_text(q, i);
            if (!strcmp(key, "context") || !strcmp(key, "snapshot") || !strcmp(key, "output"))
                v = json_tokener_parse(value);
            else
                v = json_object_new_string(value);
        }
        json_object_object_add(o, key, v);
    }
    return o;
}
static void clear(char *e, size_t n)
{
    if (e && n)
        *e = 0;
}
static int begin(Store *s, char *e, size_t n)
{
    clear(e, n);
    pthread_mutex_lock(&s->mutex);
    if (!sql(s, "BEGIN IMMEDIATE"))
        return 0;
    fail(e, n, "Workflow transaction failed: %s", sqlite3_errmsg(s->db));
    pthread_mutex_unlock(&s->mutex);
    return -1;
}
static int end(Store *s, int rc, char *e, size_t n)
{
    if (!rc && sql(s, "COMMIT"))
        rc = -1;
    if (rc) {
        if (!e || !n || !*e)
            fail(e, n, "Workflow database error: %s", sqlite3_errmsg(s->db));
        sql(s, "ROLLBACK");
    }
    pthread_mutex_unlock(&s->mutex);
    return rc;
}
static int bounded(json_object *context, json_object *output, char *e, size_t n)
{
    if (!context || !json_object_is_type(context, json_type_object))
        return fail(e, n, "Workflow context must be an object");
    if (strlen(raw(context)) + strlen(raw(output)) > WF_CONTEXT_LIMIT)
        return fail(e, n, "Workflow context/output exceeds 4 MiB");
    return 0;
}
/* Read the authoritative stored job, rather than trusting mutable worker JSON.
 * Attempts and phase are a lease token, rejecting late results after reclaim. */
static json_object *active(Store *s, json_object *job, bool *done, char *e, size_t n)
{
    *done = false;
    sqlite3_stmt *q = prepare(s, "SELECT j.*,r.status AS run_status,r.cancel_requested,r.snapshot "
                                 "FROM step_instances j JOIN runs r ON r.id=j.run_id WHERE j.id=?");
    json_object *out = NULL;
    if (q) {
        sqlite3_bind_int64(q, 1, id(job, "id"));
        if (sqlite3_step(q) == SQLITE_ROW)
            out = row(q);
    }
    sqlite3_finalize(q);
    if (!out) {
        fail(e, n, "Workflow job missing or unreadable: %s", sqlite3_errmsg(s->db));
        return NULL;
    }
    if (id(out, "run_id") != id(job, "run_id")) {
        fail(e, n, "Workflow job/run mismatch");
        goto bad;
    }
    if (!strcmp(js(out, "status", ""), "done") || !strcmp(js(out, "status", ""), "skipped")) {
        *done = true;
        return out;
    }
    if (strcmp(js(out, "run_status", ""), "running") || ji(out, "cancel_requested", 0) ||
        strcmp(js(out, "status", ""), "running") ||
        ji(out, "attempts", 0) != ji(job, "attempts", 0) ||
        ji(out, "phase", 0) != ji(job, "phase", 0)) {
        fail(e, n, "Workflow claim is stale or run is terminal");
        goto bad;
    }
    return out;
bad:
    json_object_put(out);
    return NULL;
}
static int settle(Store *s, int64_t run)
{
    return sql(s,
               "UPDATE runs SET status=CASE WHEN EXISTS(SELECT 1 FROM run_events WHERE run_id=%lld "
               "AND level='warning') THEN 'partial' ELSE 'succeeded' END,finished_at=%lld "
               "WHERE id=%lld AND status='running' AND cancel_requested=0 AND NOT EXISTS(SELECT 1 "
               "FROM step_instances WHERE run_id=%lld AND status IN('queued','running','waiting'))",
               (sqlite3_int64)run, (sqlite3_int64)now_ms(), (sqlite3_int64)run, (sqlite3_int64)run);
}
static int parent_finished(Store *s, json_object *job, size_t continuations)
{
    json_object *parent = jg(job, "parent_id");
    if (!parent)
        return 0;
    int64_t p = json_object_get_int64(parent);
    if (sql(s,
            "UPDATE step_instances SET pending=pending+%lld WHERE id=%lld AND status='waiting' "
            "AND pending>0",
            (sqlite3_int64)continuations - 1, (sqlite3_int64)p) ||
        !sqlite3_changes(s->db))
        return -1;
    return sql(s,
               "UPDATE step_instances SET status='queued',phase=1,ready_at=0 "
               "WHERE id=%lld AND status='waiting' AND pending=0",
               (sqlite3_int64)p);
}
static int insert_children(Store *s, json_object *job, json_object *children, bool fork, char *e,
                           size_t n)
{
    if (children && !json_object_is_type(children, json_type_array))
        return fail(e, n, "Workflow children must be an array");
    size_t count = children ? json_object_array_length(children) : 0;
    if (count > WF_FANOUT_LIMIT)
        return fail(e, n, "Workflow fanout exceeds 1000");
    sqlite3_stmt *q = prepare(s, "SELECT count(*) FROM step_instances WHERE run_id=?");
    if (!q)
        return -1;
    sqlite3_bind_int64(q, 1, id(job, "run_id"));
    int result = sqlite3_step(q);
    int64_t total = result == SQLITE_ROW ? sqlite3_column_int64(q, 0) : -1;
    sqlite3_finalize(q);
    if (total < 0)
        return -1;
    int max = ji(jg(job, "snapshot"), "max_steps", 1000);
    if (max < 1 || max > 10000 || count > (size_t)max || total > max - (int64_t)count)
        return fail(e, n, "Workflow max_steps limit reached");
    q = prepare(s,
                "INSERT INTO "
                "step_instances(run_id,step_id,parent_id,branch_key,record_key,context,created_at) "
                "VALUES(?,?,?,?,?,?,?)");
    if (!q)
        return -1;
    int rc = 0;
    for (size_t i = 0; i < count; i++) {
        json_object *c = json_object_array_get_idx(children, i);
        const char *step = js(c, "step_id", "");
        json_object *ctx = jg(c, "context");
        if (!*step || bounded(ctx, NULL, e, n)) {
            if (!*step)
                fail(e, n, "Workflow child has no step_id");
            rc = -1;
            break;
        }
        sqlite3_reset(q);
        sqlite3_bind_int64(q, 1, id(job, "run_id"));
        sqlite3_bind_text(q, 2, step, -1, SQLITE_TRANSIENT);
        if (fork)
            sqlite3_bind_int64(q, 3, id(job, "id"));
        else if (jg(job, "parent_id"))
            sqlite3_bind_int64(q, 3, id(job, "parent_id"));
        else
            sqlite3_bind_null(q, 3);
        sqlite3_bind_text(q, 4, fork ? js(c, "branch_key", "") : js(job, "branch_key", ""), -1,
                          SQLITE_TRANSIENT);
        sqlite3_bind_text(q, 5, js(c, "record_key", js(job, "record_key", "")), -1,
                          SQLITE_TRANSIENT);
        sqlite3_bind_text(q, 6, raw(ctx), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(q, 7, now_ms());
        if (sqlite3_step(q) != SQLITE_DONE) {
            rc = -1;
            break;
        }
    }
    sqlite3_finalize(q);
    return rc;
}
int wf_initialize(Store *s, char *e, size_t n)
{
    if (begin(s, e, n))
        return -1;
    sqlite3_stmt *q =
        prepare(s, "SELECT id,snapshot FROM runs WHERE status='queued' AND cancel_requested=0 "
                   "AND json_extract(snapshot,'$.schema_version')=2 ORDER BY id");
    json_object *runs = json_object_new_array();
    int rc = q ? 0 : -1, result = SQLITE_DONE;
    if (q)
        while ((result = sqlite3_step(q)) == SQLITE_ROW)
            json_object_array_add(runs, row(q));
    if (result != SQLITE_DONE)
        rc = -1;
    sqlite3_finalize(q);
    for (size_t i = 0; !rc && i < json_object_array_length(runs); i++) {
        json_object *r = json_object_array_get_idx(runs, i), *snapshot = jg(r, "snapshot");
        int64_t run = id(r, "id");
        const char *entry = js(snapshot, "entry", "");
        json_object *context = json_object_new_object();
        json_object_object_add(context, "run",
                               jg(snapshot, "vars") ? json_object_get(jg(snapshot, "vars"))
                                                    : json_object_new_object());
        json_object_object_add(context, "input", json_object_new_object());
        json_object_object_add(context, "steps", json_object_new_object());
        if (!*entry || bounded(context, NULL, e, n)) {
            rc = fail(e, n, "Invalid initial workflow context or entry");
        } else {
            rc = sql(s,
                     "INSERT INTO step_instances(run_id,step_id,context,record_key,created_at) "
                     "SELECT %lld,%Q,%Q,'root',%lld WHERE NOT EXISTS(SELECT 1 FROM step_instances "
                     "WHERE run_id=%lld)",
                     (sqlite3_int64)run, entry, raw(context), (sqlite3_int64)now_ms(),
                     (sqlite3_int64)run);
            if (!rc)
                rc = sql(s,
                         "UPDATE runs SET status='running',started_at=coalesce(started_at,%lld) "
                         "WHERE id=%lld",
                         (sqlite3_int64)now_ms(), (sqlite3_int64)run);
        }
        json_object_put(context);
    }
    json_object_put(runs);
    return end(s, rc, e, n);
}
json_object *wf_claim(Store *s, char *e, size_t n)
{
    if (begin(s, e, n))
        return NULL;
    sqlite3_stmt *q =
        prepare(s, "UPDATE step_instances SET status='running',attempts=attempts+1 "
                   "WHERE id=(SELECT j.id FROM step_instances j JOIN runs r ON r.id=j.run_id "
                   "WHERE j.status='queued' AND j.ready_at<=? AND r.status='running' AND "
                   "r.cancel_requested=0 ORDER BY j.id LIMIT 1) RETURNING *,"
                   "(SELECT snapshot FROM runs WHERE runs.id=step_instances.run_id) AS snapshot");
    json_object *out = NULL;
    int rc = q ? 0 : -1;
    if (q) {
        sqlite3_bind_int64(q, 1, now_ms());
        int result = sqlite3_step(q);
        if (result == SQLITE_ROW) {
            out = row(q);
            if (sqlite3_step(q) != SQLITE_DONE)
                rc = -1;
        } else if (result != SQLITE_DONE)
            rc = -1;
    }
    sqlite3_finalize(q);
    if (end(s, rc, e, n)) {
        json_object_put(out);
        return NULL;
    }
    return out;
}
static int complete(Store *s, json_object *job, json_object *output, json_object *context,
                    json_object *children, json_object *emitted, bool page, bool skipped, char *e,
                    size_t n)
{
    if (bounded(context, output, e, n) || insert_children(s, job, children, false, e, n))
        return -1;
    size_t count = children ? json_object_array_length(children) : 0;
    int added = 0;
    if (emitted) {
        if (!json_object_is_type(emitted, json_type_object))
            return fail(e, n, "Emitted record must be an object");
        if (strlen(raw(emitted)) > WF_CONTEXT_LIMIT)
            return fail(e, n, "Emitted record exceeds 4 MiB");
        if (sql(s,
                "INSERT INTO "
                "items(run_id,step_instance_id,record_key,step_id,source_url,data,created_at) "
                "VALUES(%lld,%lld,%Q,%Q,%Q,%Q,%lld) ON CONFLICT DO NOTHING",
                (sqlite3_int64)id(job, "run_id"), (sqlite3_int64)id(job, "id"),
                js(job, "record_key", ""), js(job, "step_id", ""), js(context, "source_url", ""),
                raw(emitted), (sqlite3_int64)now_ms()))
            return -1;
        added = sqlite3_changes(s->db);
    }
    if (sql(s,
            "UPDATE step_instances SET status=%Q,output=%Q,context=%Q,terminal=%d,finished_at=%lld "
            "WHERE id=%lld",
            skipped ? "skipped" : "done", raw(output), raw(context), count == 0,
            (sqlite3_int64)now_ms(), (sqlite3_int64)id(job, "id")) ||
        sql(s, "UPDATE runs SET pages=pages+%d,item_count=item_count+%d WHERE id=%lld",
            page ? 1 : 0, added, (sqlite3_int64)id(job, "run_id")) ||
        parent_finished(s, job, count))
        return -1;
    return settle(s, id(job, "run_id"));
}
int wf_finish(Store *s, json_object *job, json_object *output, json_object *context,
              json_object *children, json_object *emitted, bool page, char *e, size_t n)
{
    if (begin(s, e, n))
        return -1;
    bool done;
    json_object *stored = active(s, job, &done, e, n);
    int rc = stored ? 0 : -1;
    if (stored && !done)
        rc = complete(s, stored, output, context, children, emitted, page, false, e, n);
    json_object_put(stored);
    return end(s, rc, e, n);
}
int wf_fork(Store *s, json_object *job, json_object *children, json_object *context, char *e,
            size_t n)
{
    if (bounded(context, NULL, e, n))
        return -1;
    if (begin(s, e, n))
        return -1;
    bool done;
    json_object *stored = active(s, job, &done, e, n);
    int rc = stored ? 0 : -1;
    if (stored && !done) {
        rc = insert_children(s, stored, children, true, e, n);
        if (!rc) {
            size_t count = children ? json_object_array_length(children) : 0;
            rc = sql(s,
                     "UPDATE step_instances SET status=%Q,pending=%lld,phase=%d,context=%Q WHERE "
                     "id=%lld",
                     count ? "waiting" : "queued", (sqlite3_int64)count, count ? 0 : 1,
                     raw(context), (sqlite3_int64)id(stored, "id"));
        }
    }
    json_object_put(stored);
    return end(s, rc, e, n);
}
json_object *wf_group_results(Store *s, int64_t group, char *e, size_t n)
{
    clear(e, n);
    pthread_mutex_lock(&s->mutex);
    sqlite3_stmt *q = prepare(s, "SELECT branch_key,output FROM step_instances WHERE parent_id=? "
                                 "AND terminal=1 AND status IN('done','skipped') ORDER BY id");
    json_object *out = q ? json_object_new_object() : NULL;
    int result = SQLITE_DONE;
    size_t bytes = 2;
    if (q) {
        sqlite3_bind_int64(q, 1, group);
        while ((result = sqlite3_step(q)) == SQLITE_ROW) {
            const char *key = (const char *)sqlite3_column_text(q, 0);
            const char *value = (const char *)sqlite3_column_text(q, 1);
            json_object *encoded_key = json_object_new_string(key);
            bytes += strlen(raw(encoded_key)) + (value ? strlen(value) : 4) + 2;
            json_object_put(encoded_key);
            if (bytes > WF_CONTEXT_LIMIT) {
                fail(e, n, "Workflow group output exceeds 4 MiB");
                result = SQLITE_TOOBIG;
                break;
            }
            json_object_object_add(out, key, value ? json_tokener_parse(value) : NULL);
        }
    }
    if (!q || result != SQLITE_DONE) {
        if (!e || !n || !*e)
            fail(e, n, "Cannot read workflow group: %s", sqlite3_errmsg(s->db));
        json_object_put(out);
        out = NULL;
    }
    sqlite3_finalize(q);
    pthread_mutex_unlock(&s->mutex);
    return out;
}
int wf_failure(Store *s, json_object *job, const char *policy, const char *message, bool retry,
               int backoff_ms, char *e, size_t n)
{
    /* Workers commonly reuse their error buffer as the failure message. */
    char *saved_message = strdup(message ? message : "");
    if (!saved_message)
        return fail(e, n, "Cannot copy workflow failure message");
    message = saved_message;
    if (begin(s, e, n)) {
        free(saved_message);
        return -1;
    }
    bool done;
    json_object *stored = active(s, job, &done, e, n);
    int rc = stored ? 0 : -1;
    if (stored && !done) {
        if (!policy ||
            (strcmp(policy, "fail") && strcmp(policy, "skip") && strcmp(policy, "continue")))
            rc = fail(e, n, "Invalid workflow failure policy");
        else if (retry)
            rc = sql(
                s, "UPDATE step_instances SET status='queued',error=%Q,ready_at=%lld WHERE id=%lld",
                message ? message : "",
                (sqlite3_int64)(now_ms() + (backoff_ms > 0 ? backoff_ms : 0)),
                (sqlite3_int64)id(stored, "id"));
        else if (!strcmp(policy, "fail")) {
            rc = sql(s,
                     "UPDATE runs SET status='failed',error=%Q,finished_at=%lld WHERE id=%lld;"
                     "UPDATE step_instances SET status=CASE WHEN id=%lld THEN 'failed' ELSE "
                     "'cancelled' END,error=CASE WHEN id=%lld THEN %Q ELSE error END "
                     "WHERE run_id=%lld AND status IN('queued','running','waiting')",
                     message ? message : "", (sqlite3_int64)now_ms(),
                     (sqlite3_int64)id(stored, "run_id"), (sqlite3_int64)id(stored, "id"),
                     (sqlite3_int64)id(stored, "id"), message ? message : "",
                     (sqlite3_int64)id(stored, "run_id"));
        } else {
            rc = sql(s,
                     "UPDATE step_instances SET error=%Q WHERE id=%lld;"
                     "INSERT INTO run_events(run_id,level,message,created_at) "
                     "VALUES(%lld,'warning',%Q,%lld)",
                     message ? message : "", (sqlite3_int64)id(stored, "id"),
                     (sqlite3_int64)id(stored, "run_id"), message ? message : "",
                     (sqlite3_int64)now_ms());
            if (!rc && !strcmp(policy, "skip")) {
                json_object *output = json_object_new_object();
                json_object_object_add(output, "error",
                                       json_object_new_string(message ? message : ""));
                rc = complete(s, stored, output, jg(stored, "context"), NULL, NULL, false, true, e,
                              n);
                json_object_put(output);
            }
        }
    }
    json_object_put(stored);
    free(saved_message);
    return end(s, rc, e, n);
}
int wf_cancel(Store *s, int64_t run, char *e, size_t n)
{
    if (begin(s, e, n))
        return -1;
    int rc =
        sql(s,
            "UPDATE runs SET cancel_requested=1,status='cancelled',finished_at=%lld WHERE id=%lld "
            "AND status IN('queued','running');UPDATE step_instances SET status='cancelled' "
            "WHERE run_id=%lld AND status IN('queued','running','waiting') AND EXISTS(SELECT 1 "
            "FROM runs "
            "WHERE id=%lld AND status='cancelled')",
            (sqlite3_int64)now_ms(), (sqlite3_int64)run, (sqlite3_int64)run, (sqlite3_int64)run);
    return end(s, rc, e, n);
}
