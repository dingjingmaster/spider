#include "workflow.h"
#include "rule/workflow.h"
#include "rule/rule.h"
#include "rule/data.h"
#include "common/redact.h"
#include "storage/workflow.h"
#include "http/http.h"
#include "extract/extract.h"
#include "artifact/artifact.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct {
    App *app;
    int64_t run;
} FlowExecution;
static int halted(void *context)
{
    FlowExecution *x = context;
    if (atomic_load(&x->app->stopping))
        return 1;
    if (!x->run)
        return 0;
    json_object *rows = store_query(
        &x->app->store, "SELECT status,cancel_requested FROM runs WHERE id=?", x->run, NULL, 0);
    bool stop = true;
    if (rows && json_object_array_length(rows)) {
        json_object *r = json_object_array_get_idx(rows, 0);
        stop = ji(r, "cancel_requested", 0) || strcmp(js(r, "status", ""), "running");
    }
    json_object_put(rows);
    return stop;
}
static int pause_step(FlowExecution *x, int ms)
{
    while (ms > 0) {
        if (halted(x))
            return -1;
        int piece = ms > 100 ? 100 : ms;
        sleep_ms(piece);
        ms -= piece;
    }
    return halted(x) ? -1 : 0;
}
json_object *workflow_context(json_object *rule, json_object *supplied)
{
    if (supplied && !json_object_is_type(supplied, json_type_object))
        return NULL;
    json_object *ctx = flow_clone(supplied);
    if (!ctx || !json_object_is_type(ctx, json_type_object)) {
        json_object_put(ctx);
        ctx = json_object_new_object();
    }
    json_object *ignored = NULL;
    if (!json_object_object_get_ex(ctx, "run", &ignored))
        json_object_object_add(
            ctx, "run", jg(rule, "vars") ? flow_clone(jg(rule, "vars")) : json_object_new_object());
    if (!json_object_object_get_ex(ctx, "input", &ignored))
        json_object_object_add(ctx, "input", json_object_new_object());
    if (!json_object_object_get_ex(ctx, "steps", &ignored))
        json_object_object_add(ctx, "steps", json_object_new_object());
    if (!json_object_is_type(jg(ctx, "run"), json_type_object) ||
        !json_object_is_type(jg(ctx, "steps"), json_type_object)) {
        json_object_put(ctx);
        return NULL;
    }
    return ctx;
}
static void options(json_object *out, json_object *source)
{
    const char *keys[] = {"timeout_seconds", "retries", "delay_ms", "max_bytes"};
    for (size_t i = 0; i < 4; i++) {
        json_object *v = jg(source, keys[i]);
        if (v)
            json_object_object_add(out, keys[i], flow_clone(v));
    }
}
static int add_query(json_object *request, char *e, size_t n)
{
    json_object *query = jg(request, "query");
    if (!query || !json_object_object_length(query))
        return 0;
    CURLU *u = curl_url();
    CURL *escape = curl_easy_init();
    char *url = NULL, *old = NULL, *buffer = NULL;
    size_t size = 0;
    FILE *f = open_memstream(&buffer, &size);
    int rc = -1;
    if (!u || !escape || !f ||
        curl_url_set(u, CURLUPART_URL, js(request, "url", ""), 0) != CURLUE_OK)
        goto done;
    if (curl_url_get(u, CURLUPART_QUERY, &old, 0) == CURLUE_OK)
        fputs(old, f);
    json_object_object_foreach(query, key, value)
    {
        if (!value)
            continue;
        if (json_object_is_type(value, json_type_array) ||
            json_object_is_type(value, json_type_object)) {
            fail(e, n, "Query values must be scalar");
            goto done;
        }
        char *k = curl_easy_escape(escape, key, 0),
             *v = curl_easy_escape(escape, json_object_get_string(value), 0);
        if (!k || !v) {
            curl_free(k);
            curl_free(v);
            goto done;
        }
        if (ftell(f) > 0)
            fputc('&', f);
        fprintf(f, "%s=%s", k, v);
        curl_free(k);
        curl_free(v);
    }
    if (fclose(f)) {
        f = NULL;
        goto done;
    }
    f = NULL;
    if (curl_url_set(u, CURLUPART_QUERY, buffer, 0) != CURLUE_OK ||
        curl_url_get(u, CURLUPART_URL, &url, 0) != CURLUE_OK)
        goto done;
    json_object_object_add(request, "url", json_object_new_string(url));
    rc = 0;
done:
    if (f)
        fclose(f);
    free(buffer);
    curl_free(url);
    curl_free(old);
    curl_url_cleanup(u);
    curl_easy_cleanup(escape);
    if (rc && (!e || !*e))
        fail(e, n, "Cannot construct query URL");
    return rc;
}
json_object *workflow_request(json_object *rule, json_object *step, json_object *context, char *e,
                              size_t n)
{
    json_object *rendered = NULL;
    if (flow_render(jg(step, "request"), context, &rendered, e, n))
        return NULL;
    if (!rendered || !json_object_is_type(rendered, json_type_object)) {
        json_object_put(rendered);
        fail(e, n, "Missing request object");
        return NULL;
    }
    json_object *out = json_object_new_object();
    options(out, jg(rule, "defaults"));
    options(out, step);
    json_object_object_foreach(rendered, k, v)
    {
        json_object_object_add(out, k, flow_clone(v));
    }
    json_object_put(rendered);
    const char *url = js(out, "url", "");
    if (!*url) {
        fail(e, n, "Request URL is empty");
        goto bad;
    }
    if (!url_valid(url)) {
        char *resolved = url_resolve(js(context, "source_url", ""), url);
        if (!resolved) {
            fail(e, n, "Cannot resolve request URL");
            goto bad;
        }
        json_object_object_add(out, "url", json_object_new_string(resolved));
        free(resolved);
    }
    if (add_query(out, e, n))
        goto bad;
    json_object *body_json = NULL;
    if (json_object_object_get_ex(out, "json", &body_json)) {
        json_object_object_add(out, "body",
                               json_object_new_string(json_object_to_json_string_ext(
                                   body_json, JSON_C_TO_STRING_PLAIN)));
        json_object *h = jg(out, "headers");
        if (!h) {
            h = json_object_new_object();
            json_object_object_add(out, "headers", h);
        }
        bool has = false;
        json_object_object_foreach(h, key, value)
        {
            (void)value;
            if (!strcasecmp(key, "Content-Type"))
                has = true;
        }
        if (!has)
            json_object_object_add(h, "Content-Type", json_object_new_string("application/json"));
    }
    /* Shared validation of rendered HTTP options and extraction. */
    json_object_object_add(out, "name", json_object_new_string(js(step, "id", "step")));
    json_object *extract = jg(step, "extract");
    if (extract) {
        json_object_object_foreach(extract, field_key, field_value)
        {
            json_object_object_add(out, field_key, flow_clone(field_value));
        }
    } else
        json_object_object_add(out, "fields",
                               json_tokener_parse("[{\"name\":\"_body\",\"selector\":\"\"}]"));
    if (request_validate(out, e, n))
        goto bad;
    return out;
bad:
    json_object_put(out);
    return NULL;
}
json_object *workflow_response(json_object *step, json_object *request, HttpResponse *r, char *e,
                               size_t n)
{
    json_object *items = NULL;
    char *next = NULL;
    if (jg(step, "extract")) {
        if (extract_document(request, r->body, r->url, &items, &next, e, n))
            return NULL;
    } else
        items = json_object_new_array();
    json_object *out = json_object_new_object(), *response = json_object_new_object();
    json_object_object_add(out, "items", items);
    json_object_object_add(out, "next_url", next ? json_object_new_string(next) : NULL);
    free(next);
    json_object_object_add(response, "status", json_object_new_int64(r->status));
    json_object_object_add(response, "url", json_object_new_string(r->url));
    if (jb(step, "response_json", false)) {
        json_object *value = NULL;
        if (json_value_parse(r->body, &value, e, n)) {
            json_object_put(response);
            json_object_put(out);
            return NULL;
        }
        json_object_object_add(response, "json", value);
    }
    if (jb(step, "response_body", true))
        json_object_object_add(
            response, "body",
            json_object_new_string_len(r->body, r->size > 65536 ? 65536 : (int)r->size));
    json_object_object_add(response, "bytes", json_object_new_int64((int64_t)r->size));
    json_object_object_add(
        response, "truncated",
        json_object_new_boolean(jb(step, "response_body", true) && r->size > 65536));
    json_object_object_add(out, "response", response);
    return out;
}
static json_object *child(const char *step, json_object *context, const char *record,
                          const char *branch)
{
    json_object *c = json_object_new_object();
    json_object_object_add(c, "step_id", json_object_new_string(step));
    json_object_object_add(c, "context", flow_clone(context));
    json_object_object_add(c, "record_key", json_object_new_string(record));
    json_object_object_add(c, "branch_key", json_object_new_string(branch));
    return c;
}
static json_object *continuation(json_object *job, json_object *context, const char *next)
{
    json_object *children = json_object_new_array();
    if (next && *next)
        json_object_array_add(children, child(next, context, js(job, "record_key", "root"),
                                              js(job, "branch_key", "")));
    return children;
}
static int update_context(json_object *context, const char *step, json_object *output, char *e,
                          size_t n)
{
    json_object *steps = jg(context, "steps");
    if (!steps || !json_object_is_type(steps, json_type_object))
        return fail(e, n, "context.steps must be an object");
    json_object_object_add(steps, step, flow_clone(output));
    return 0;
}
static int next_input(json_object *step, json_object *context, char *e, size_t n)
{
    json_object *expr = NULL;
    if (!json_object_object_get_ex(step, "next_input", &expr))
        return 0;
    json_object *value = NULL;
    if (flow_render(expr, context, &value, e, n))
        return -1;
    json_object_object_add(context, "input", value);
    return 0;
}
int workflow_complete_context(json_object *rule, json_object *step, json_object *context,
                              json_object *output, char *e, size_t n)
{
    if (debug_track_secrets(context, output, rule, e, n))
        return -1;
    if (update_context(context, js(step, "id", ""), output, e, n) ||
        next_input(step, context, e, n))
        return -1;
    json_object *keep = jg(step, "retain_steps");
    if (keep) {
        json_object *old = jg(context, "steps"), *retained = json_object_new_object();
        for (size_t i = 0; i < json_object_array_length(keep); i++) {
            const char *id = json_object_get_string(json_object_array_get_idx(keep, i));
            json_object *value = NULL;
            if (json_object_object_get_ex(old, id, &value))
                json_object_object_add(retained, id, flow_clone(value));
        }
        json_object_object_add(context, "steps", retained);
    }
    return 0;
}
static void handle_failure(App *app, json_object *job, json_object *step, json_object *context,
                           const char *message, bool retry)
{
    if (atomic_load(&app->stopping))
        return;
    FlowExecution execution = {app, json_object_get_int64(jg(job, "run_id"))};
    if (halted(&execution))
        return;
    char e[SP_ERROR] = {0};
    const char *policy = js(step, "on_error", "fail");
    if (retry) {
        if (wf_failure(&app->store, job, policy, message, true, 500, e, sizeof(e)))
            fprintf(stderr, "Workflow retry persistence failed: %s\n", e);
        return;
    }
    if (!strcmp(policy, "continue")) {
        if (wf_failure(&app->store, job, policy, message, false, 0, e, sizeof(e)))
            goto fatal;
        json_object *out = json_object_new_object();
        json_object_object_add(out, "error", json_object_new_string(message));
        if (workflow_complete_context(jg(job, "snapshot"), step, context, out, e, sizeof(e))) {
            json_object_put(out);
            goto fatal;
        }
        json_object *next = continuation(job, context, js(step, "next", ""));
        int rc = wf_finish(&app->store, job, out, context, next, NULL, false, e, sizeof(e));
        json_object_put(next);
        json_object_put(out);
        if (rc)
            goto fatal;
    } else if (wf_failure(&app->store, job, policy, message, false, 0, e, sizeof(e)))
        goto fatal;
    return;
fatal:
    if (wf_failure(&app->store, job, "fail", e, false, 0, NULL, 0))
        fprintf(stderr, "Workflow error persistence failed\n");
}
static int fork_step(App *app, json_object *job, json_object *step, json_object *ctx, char *e,
                     size_t n)
{
    json_object *children = json_object_new_array(), *values = NULL;
    const char *type = js(step, "type", "");
    size_t total = 0;
    int rc = -1;
    char record[128], branch[40];
    if (!strcmp(type, "foreach")) {
        if (flow_render(jg(step, "items"), ctx, &values, e, n))
            goto done;
        if (!values || !json_object_is_type(values, json_type_array) ||
            json_object_array_length(values) > 1000) {
            fail(e, n, "foreach requires an array of at most 1000 elements");
            goto done;
        }
        for (size_t i = 0; i < json_object_array_length(values); i++) {
            json_object *copy = flow_clone(ctx);
            json_object_object_add(copy, "input", flow_clone(json_object_array_get_idx(values, i)));
            snprintf(record, sizeof(record), "%lld:%zu",
                     (long long)json_object_get_int64(jg(job, "id")), i);
            snprintf(branch, sizeof(branch), "%zu", i);
            json_object *c = child(js(step, "body", ""), copy, record, branch);
            json_object_put(copy);
            total += strlen(json_object_to_json_string_ext(c, JSON_C_TO_STRING_PLAIN));
            if (total > 16777216) {
                json_object_put(c);
                fail(e, n, "Fork contexts exceed 16 MiB");
                goto done;
            }
            json_object_array_add(children, c);
        }
    } else {
        json_object *branches = jg(step, "branches");
        json_object_object_foreach(branches, key, value)
        {
            json_object *c =
                child(json_object_get_string(value), ctx, js(job, "record_key", "root"), key);
            total += strlen(json_object_to_json_string_ext(c, JSON_C_TO_STRING_PLAIN));
            if (total > 16777216) {
                json_object_put(c);
                fail(e, n, "Fork contexts exceed 16 MiB");
                goto done;
            }
            json_object_array_add(children, c);
        }
    }
    rc = wf_fork(&app->store, job, children, ctx, e, n);
done:
    json_object_put(values);
    json_object_put(children);
    return rc;
}
static int group_output(App *app, json_object *job, json_object *step, json_object **out, char *e,
                        size_t n)
{
    json_object *results =
        wf_group_results(&app->store, json_object_get_int64(jg(job, "id")), e, n);
    if (!results)
        return -1;
    if (!strcmp(js(step, "type", ""), "foreach")) {
        json_object *array = json_object_new_array();
        size_t count = json_object_object_length(results);
        for (size_t i = 0; i < count; i++) {
            char key[40];
            snprintf(key, sizeof(key), "%zu", i);
            json_object *v = NULL;
            if (!json_object_object_get_ex(results, key, &v)) {
                json_object_put(array);
                json_object_put(results);
                return fail(e, n, "Incomplete foreach group results");
            }
            json_object_array_add(array, flow_clone(v));
        }
        json_object_put(results);
        *out = array;
    } else
        *out = results;
    return 0;
}
/* Persist actual resolved HTTP request identity, without authentication headers/body. */
static int record_request(App *app, json_object *job, json_object *request, const char *status,
                          char *e, size_t n)
{
    Store *s = &app->store;
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
void workflow_execute(App *app, json_object *job)
{
    json_object *rule = jg(job, "snapshot"), *step = flow_step(rule, js(job, "step_id", "")),
                *ctx = flow_clone(jg(job, "context")), *out = NULL, *request = NULL, *next = NULL;
    int64_t run = json_object_get_int64(jg(job, "run_id"));
    FlowExecution execution = {app, run};
    char e[SP_ERROR] = {0};
    bool page = false, retry = false;
    HttpSession *session = NULL;
    if (!step || !ctx) {
        handle_failure(app, job, step, ctx, "Missing workflow step/context", false);
        goto done;
    }
    if (halted(&execution))
        goto done;
    const char *type = js(step, "type", ""), *target = js(step, "next", "");
    if (!ji(job, "phase", 0) && (debug_track_secrets(ctx, NULL, rule, e, sizeof(e)) ||
                                 flow_input_prepare(step, ctx, e, sizeof(e))))
        goto failed;
    if ((!strcmp(type, "foreach") || !strcmp(type, "parallel")) && !ji(job, "phase", 0)) {
        if (fork_step(app, job, step, ctx, e, sizeof(e)))
            handle_failure(app, job, step, ctx, e, false);
        goto done;
    }
    if (ji(job, "phase", 0)) {
        if (group_output(app, job, step, &out, e, sizeof(e)))
            goto failed;
    } else if (!strcmp(type, "request")) {
        request = workflow_request(rule, step, ctx, e, sizeof(e));
        if (!request)
            goto failed;
        if (debug_track_secrets(ctx, request, rule, e, sizeof(e)))
            goto failed;
        if (!strcmp(js(request, "method", "GET"), "POST") && !jb(step, "retry_post", false)) {
            json_object *previous = store_query(
                &app->store,
                "SELECT id FROM requests WHERE step_instance_id=? AND method='POST' LIMIT 1",
                json_object_get_int64(jg(job, "id")), e, sizeof(e));
            if (!previous)
                goto failed;
            bool uncertain = json_object_array_length(previous) > 0;
            json_object_put(previous);
            if (uncertain) {
                fail(e, sizeof(e),
                     "Interrupted POST has an uncertain result; explicit retry_post is required to "
                     "replay");
                goto failed;
            }
        }
        session = session_acquire(app->sessions, run, e, sizeof(e));
        if (!session) {
            if (!*e)
                fail(e, sizeof(e), "Cannot initialize session");
            goto failed;
        }
        if (pause_step(&execution, ji(request, "delay_ms", 300)))
            goto done;
        if (record_request(app, job, request, "running", e, sizeof(e)))
            goto failed;
        HttpResponse response = {0};
        int rc = http_fetch_session(request, js(request, "url", ""), NULL, halted, &execution,
                                    session, &response, e, sizeof(e));
        char db_error[SP_ERROR] = {0};
        if (!atomic_load(&app->stopping) &&
            record_request(app, job, request, rc ? "failed" : "succeeded", db_error,
                           sizeof(db_error))) {
            snprintf(e, sizeof(e), "%s", db_error);
            rc = -1;
        }
        if (rc) {
            http_response_free(&response);
            retry = ji(job, "attempts", 1) <= ji(request, "retries", 2) &&
                    (!strcmp(js(request, "method", "GET"), "GET") || jb(step, "retry_post", false));
            goto failed;
        }
        out = workflow_response(step, request, &response, e, sizeof(e));
        if (out)
            json_object_object_add(ctx, "source_url", json_object_new_string(response.url));
        http_response_free(&response);
        if (!out)
            goto failed;
        json_object *counts = store_query(
            &app->store,
            "SELECT count(*) AS iteration FROM step_instances a JOIN step_instances b ON b.id=? "
            "WHERE a.run_id=b.run_id AND a.step_id=b.step_id AND a.record_key=b.record_key "
            "AND a.branch_key=b.branch_key AND a.id<=b.id",
            json_object_get_int64(jg(job, "id")), e, sizeof(e));
        if (!counts)
            goto failed;
        json_object_object_add(out, "iteration",
                               flow_clone(jg(json_object_array_get_idx(counts, 0), "iteration")));
        json_object_put(counts);
        page = true;
    } else if (!strcmp(type, "transform")) {
        if (flow_transform(step, ctx, &out, e, sizeof(e)) ||
            debug_track_transform(ctx, step, out, rule, e, sizeof(e)))
            goto failed;
    } else if (!strcmp(type, "condition")) {
        bool result = false;
        if (flow_test(jg(step, "test"), ctx, &result, e, sizeof(e)))
            goto failed;
        out = json_object_new_object();
        json_object_object_add(out, "result", json_object_new_boolean(result));
        target = js(step, result ? "then" : "else", "");
    } else if (!strcmp(type, "emit") || !strcmp(type, "join")) {
        if (flow_render(jg(step, "fields"), ctx, &out, e, sizeof(e)))
            goto failed;
    } else if (!strcmp(type, "download")) {
        json_object *download = flow_clone(step), *spec = json_object_new_object();
        const char *keys[] = {"url", "headers", "headers_origin"};
        for (size_t i = 0; i < sizeof(keys) / sizeof(*keys); i++)
            if (jg(step, keys[i]))
                json_object_object_add(spec, keys[i], flow_clone(jg(step, keys[i])));
        json_object_object_add(download, "request", spec);
        request = workflow_request(rule, download, ctx, e, sizeof(e));
        json_object_put(download);
        if (!request)
            goto failed;
        if (debug_track_secrets(ctx, request, rule, e, sizeof(e)))
            goto failed;
        json_object *value = flow_clone(jg(request, "url"));
        session = session_acquire(app->sessions, run, e, sizeof(e));
        int rc = session
                     ? artifact_download_session(app, run, request, json_object_get_string(value),
                                                 halted, &execution, session, e, sizeof(e))
                     : -1;
        if (!rc) {
            json_object *rows = store_query(
                &app->store,
                "SELECT id,url,sha256,bytes FROM artifacts WHERE run_id=? ORDER BY id DESC", run, e,
                sizeof(e));
            if (rows) {
                for (size_t i = 0; i < json_object_array_length(rows); i++) {
                    json_object *a = json_object_array_get_idx(rows, i);
                    if (!strcmp(js(a, "url", ""), json_object_get_string(value))) {
                        out = flow_clone(a);
                        break;
                    }
                }
                json_object_put(rows);
            }
            if (!out) {
                fail(e, sizeof(e), "Downloaded file metadata missing");
                rc = -1;
            }
        }
        json_object_put(value);
        if (rc) {
            retry = ji(job, "attempts", 1) <= ji(request, "retries", 2);
            goto failed;
        }
    } else {
        fail(e, sizeof(e), "Unknown step type");
        goto failed;
    }
    if (workflow_complete_context(rule, step, ctx, out, e, sizeof(e)))
        goto failed;
    next = continuation(job, ctx, target);
    if (wf_finish(&app->store, job, out, ctx, next, !strcmp(type, "emit") ? out : NULL, page, e,
                  sizeof(e)))
        goto failed;
    goto done;
failed:
    handle_failure(app, job, step, ctx, *e ? e : "Workflow execution failed", retry);
done:
    session_release(app->sessions, session);
    json_object_put(next);
    json_object_put(out);
    json_object_put(request);
    json_object_put(ctx);
    session_pool_prune(app->sessions, run);
}
json_object *workflow_preview(App *app, json_object *payload, char *e, size_t n)
{
    json_object *rule = jg(payload, "rule");
    if (!rule)
        rule = payload;
    if (rule_validate(rule, e, n))
        return NULL;
    const char *step_id = js(payload, "step_id", js(rule, "entry", ""));
    json_object *step = flow_step(rule, step_id);
    if (!step || strcmp(js(step, "type", ""), "request")) {
        fail(e, n, "Preview requires a request step");
        return NULL;
    }
    json_object *ctx = workflow_context(rule, jg(payload, "context")), *request = NULL, *out = NULL;
    if (!ctx || debug_track_secrets(ctx, NULL, rule, e, n) || flow_input_prepare(step, ctx, e, n)) {
        if (!ctx)
            fail(e, n, "Preview context.run and context.steps must be objects");
        json_object_put(ctx);
        return NULL;
    }
    request = workflow_request(rule, step, ctx, e, n);
    HttpSession *session = session_temporary();
    HttpResponse response = {0};
    FlowExecution execution = {app, 0};
    if (request && session && !debug_track_secrets(ctx, request, rule, e, n) &&
        !http_fetch_session(request, js(request, "url", ""), NULL, halted, &execution, session,
                            &response, e, n))
        out = workflow_response(step, request, &response, e, n);
    if (out) {
        json_object *safe = debug_redact_with_context(out, rule, ctx);
        json_object_put(out);
        out = safe;
    }
    http_response_free(&response);
    session_destroy(session);
    json_object_put(request);
    json_object_put(ctx);
    return out;
}
