#include "web.h"
#include "engine/workflow.h"
#include "engine/preview.h"
#include "common/redact.h"
#include "rule/rule.h"
#include <civetweb.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

static int send_bytes(struct mg_connection *c, int status, const char *type, const char *body,
                      size_t len)
{
    mg_printf(c,
              "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: "
              "no-store\r\nX-Content-Type-Options: nosniff\r\nReferrer-Policy: "
              "no-referrer\r\nContent-Security-Policy: default-src 'self'; script-src 'self'; "
              "style-src 'self'; object-src 'none'; base-uri 'none'; frame-ancestors "
              "'none'\r\nConnection: close\r\n\r\n",
              status, status < 400 ? "OK" : "Error", type, len);
    if (len)
        mg_write(c, body, len);
    return status;
}
static int send_json(struct mg_connection *c, int status, json_object *j)
{
    const char *body = json_object_to_json_string_ext(j, JSON_C_TO_STRING_PLAIN);
    int rc = send_bytes(c, status, "application/json; charset=utf-8", body, strlen(body));
    json_object_put(j);
    return rc;
}
static int error_json(struct mg_connection *c, int status, const char *error)
{
    json_object *j = json_object_new_object();
    json_object_object_add(j, "error", json_object_new_string(error));
    return send_json(c, status, j);
}
static json_object *id_json(int64_t id)
{
    json_object *j = json_object_new_object();
    json_object_object_add(j, "id", json_object_new_int64(id));
    return j;
}
static bool token_equal(const char *a, const char *b)
{
    size_t al = strlen(a), bl = strlen(b);
    unsigned diff = (unsigned)(al ^ bl);
    for (size_t i = 0; i < bl; i++)
        diff |= (unsigned char)(i < al ? a[i] : 0) ^ (unsigned char)b[i];
    return diff == 0;
}
static json_object *read_json(struct mg_connection *c, char *e, size_t cap)
{
    const struct mg_request_info *r = mg_get_request_info(c);
    if (r->content_length <= 0 || r->content_length > 1048576) {
        fail(e, cap, "JSON body must contain 1-1048576 bytes");
        return NULL;
    }
    size_t len = (size_t)r->content_length;
    char *body = malloc(len + 1);
    if (!body)
        return NULL;
    size_t got = 0;
    while (got < len) {
        int n = mg_read(c, body + got, len - got);
        if (n <= 0) {
            free(body);
            fail(e, cap, "Incomplete request body");
            return NULL;
        }
        got += (size_t)n;
    }
    body[len] = 0;
    if (memchr(body, 0, len)) {
        free(body);
        fail(e, cap, "NUL byte in JSON body");
        return NULL;
    }
    json_object *j = json_parse(body, e, cap);
    free(body);
    return j;
}
static json_object *one_row(App *a, const char *sql, int64_t id)
{
    json_object *rows = store_query(&a->store, sql, id, NULL, 0), *out = NULL;
    if (rows && json_object_array_length(rows))
        out = json_object_get(json_object_array_get_idx(rows, 0));
    if (rows)
        json_object_put(rows);
    return out;
}
static int query_reply(struct mg_connection *c, App *a, const char *sql, int64_t id)
{
    char e[SP_ERROR] = {0};
    json_object *rows = store_query(&a->store, sql, id, e, sizeof(e));
    return rows ? send_json(c, 200, rows) : error_json(c, 500, e);
}
static void csv_cell(FILE *f, const char *s)
{
    fputc('"', f);
    if (*s == '=' || *s == '+' || *s == '-' || *s == '@' || *s == '\t' || *s == '\r')
        fputc('\'', f);
    for (; *s; s++) {
        if (*s == '"')
            fputc('"', f);
        fputc(*s, f);
    }
    fputc('"', f);
}
static bool csv_fields_valid(json_object *fields)
{
    if (!fields || !json_object_is_type(fields, json_type_array) ||
        !json_object_array_length(fields))
        return false;
    for (size_t i = 0; i < json_object_array_length(fields); i++) {
        json_object *field = json_object_array_get_idx(fields, i);
        if (!field || !json_object_is_type(field, json_type_object))
            return false;
        json_object *name = jg(field, "name");
        if (!name || !json_object_is_type(name, json_type_string) || !*json_object_get_string(name))
            return false;
    }
    return true;
}
static int export_run(struct mg_connection *c, App *app, int64_t id, bool csv)
{
    json_object *run = one_row(app, "SELECT snapshot FROM runs WHERE id=?", id);
    if (!run)
        return error_json(c, 404, "Run not found");
    char *body = NULL;
    size_t size = 0;
    FILE *f = open_memstream(&body, &size);
    if (!f) {
        json_object_put(run);
        return error_json(c, 500, "Cannot create export");
    }
    pthread_mutex_lock(&app->store.mutex);
    json_object *snapshot = jg(run, "snapshot");
    json_object *fields = jg(snapshot, "fields"), *derived_fields = NULL;
    bool explicit_fields = json_object_object_get_ex(snapshot, "fields", &fields);
    if (csv && ji(snapshot, "schema_version", 1) == 2 && explicit_fields &&
        !csv_fields_valid(fields)) {
        pthread_mutex_unlock(&app->store.mutex);
        fclose(f);
        free(body);
        json_object_put(run);
        return error_json(c, 422,
                          "Workflow fields must be a nonempty array of objects with name strings");
    }
    /* Explicit export fields select/order columns; otherwise infer emitted record keys. */
    if (csv && ji(snapshot, "schema_version", 1) == 2 && !explicit_fields) {
        derived_fields = json_object_new_array();
        sqlite3_stmt *columns = NULL;
        int cr =
            sqlite3_prepare_v2(app->store.db,
                               "SELECT DISTINCT json_each.key FROM items,json_each(items.data) "
                               "WHERE items.run_id=? ORDER BY json_each.key",
                               -1, &columns, NULL);
        if (cr == SQLITE_OK) {
            sqlite3_bind_int64(columns, 1, id);
            while ((cr = sqlite3_step(columns)) == SQLITE_ROW) {
                json_object *field = json_object_new_object();
                json_object_object_add(
                    field, "name",
                    json_object_new_string((const char *)sqlite3_column_text(columns, 0)));
                json_object_array_add(derived_fields, field);
            }
        }
        sqlite3_finalize(columns);
        if (cr != SQLITE_DONE) {
            pthread_mutex_unlock(&app->store.mutex);
            fclose(f);
            free(body);
            json_object_put(derived_fields);
            json_object_put(run);
            return error_json(c, 500, "Cannot derive export columns");
        }
        fields = derived_fields;
    }
    if (csv) {
        for (size_t i = 0; i < json_object_array_length(fields); i++) {
            if (i)
                fputc(',', f);
            csv_cell(f, js(json_object_array_get_idx(fields, i), "name", ""));
        }
        fputs("\r\n", f);
    } else
        fputc('[', f);
    sqlite3_stmt *q = NULL;
    int rc = sqlite3_prepare_v2(app->store.db, "SELECT data FROM items WHERE run_id=? ORDER BY id",
                                -1, &q, NULL);
    size_t count = 0;
    bool too_large = false;
    if (rc == SQLITE_OK) {
        sqlite3_bind_int64(q, 1, id);
        while ((rc = sqlite3_step(q)) == SQLITE_ROW) {
            const char *text = (const char *)sqlite3_column_text(q, 0);
            if (csv) {
                json_object *item = json_tokener_parse(text);
                for (size_t i = 0; i < json_object_array_length(fields); i++) {
                    if (i)
                        fputc(',', f);
                    json_object *v = jg(item, js(json_object_array_get_idx(fields, i), "name", ""));
                    csv_cell(f, v ? json_object_get_string(v) : "");
                }
                fputs("\r\n", f);
                json_object_put(item);
            } else {
                if (count)
                    fputc(',', f);
                fputs(text, f);
            }
            count++;
            if (ftell(f) > 33554432) {
                too_large = true;
                break;
            }
        }
    }
    sqlite3_finalize(q);
    pthread_mutex_unlock(&app->store.mutex);
    if (!csv)
        fputc(']', f);
    int io = fclose(f);
    if (derived_fields)
        json_object_put(derived_fields);
    json_object_put(run);
    if (io || too_large || rc != SQLITE_DONE) {
        free(body);
        return error_json(c, too_large ? 413 : 500,
                          too_large ? "Export exceeds 32 MiB; use paginated results API"
                                    : "Export failed");
    }
    int status = send_bytes(
        c, 200, csv ? "text/csv; charset=utf-8" : "application/json; charset=utf-8", body, size);
    free(body);
    return status;
}
static int64_t query_number(const char *query, const char *key, int64_t fallback)
{
    char b[64];
    if (!query || mg_get_var(query, strlen(query), key, b, sizeof(b)) < 0)
        return fallback;
    errno = 0;
    char *end;
    long long v = strtoll(b, &end, 10);
    return errno || *end || v < 0 ? fallback : v;
}
static int handler(struct mg_connection *c, void *context)
{
    App *app = context;
    const struct mg_request_info *r = mg_get_request_info(c);
    const char *path = r->local_uri, *method = r->request_method;
    char e[SP_ERROR] = {0};
    bool get = !strcmp(method, "GET"), post = !strcmp(method, "POST"), put = !strcmp(method, "PUT"),
         del = !strcmp(method, "DELETE");
    if (!strncmp(path, "/api/", 5)) {
        if (app->token[0]) {
            const char *auth = mg_get_header(c, "Authorization");
            if (!auth || strncmp(auth, "Bearer ", 7) || !token_equal(auth + 7, app->token))
                return error_json(c, 401, "API token required");
        }
        if (!get) {
            const char *marker = mg_get_header(c, "X-Spider-Request");
            if (!marker || strcmp(marker, "1"))
                return error_json(c, 403, "X-Spider-Request: 1 required");
        }
        if (get && !strcmp(path, "/api/health")) {
            json_object *j = json_object_new_object();
            json_object_object_add(j, "version", json_object_new_string("0.1.0"));
            json_object_object_add(j, "browser_available", json_object_new_boolean(false));
            return send_json(c, 200, j);
        }
        if (post && !strcmp(path, "/api/preview/release")) {
            json_object *payload = read_json(c, e, sizeof(e));
            if (!payload)
                return error_json(c, 400, e);
            int rc =
                preview_pool_release(app->previews, js(payload, "session_id", ""), e, sizeof(e));
            json_object_put(payload);
            if (rc)
                return error_json(c, 404, e);
            json_object *reply = json_object_new_object();
            json_object_object_add(reply, "released", json_object_new_boolean(true));
            return send_json(c, 200, reply);
        }
        if ((!strcmp(path, "/api/preview") || !strcmp(path, "/api/preview/step")) && post) {
            json_object *rule = read_json(c, e, sizeof(e));
            if (!rule)
                return error_json(c, 400, e);
            json_object *definition = jg(rule, "rule");
            if (rule_validate(definition ? definition : rule, e, sizeof(e))) {
                json_object_put(rule);
                return error_json(c, 400, e);
            }
            json_object *out = !strcmp(path, "/api/preview/step")
                                   ? workflow_preview_step(app, rule, e, sizeof(e))
                                   : workflow_preview(app, rule, e, sizeof(e));
            if (out && !strcmp(path, "/api/preview")) {
                json_object *safe = debug_redact(out, definition ? definition : rule);
                json_object_put(out);
                out = safe;
            }
            json_object_put(rule);
            return out ? send_json(c, 200, out) : error_json(c, 422, e);
        }
        if (!strcmp(path, "/api/spiders")) {
            if (get)
                return query_reply(
                    c, app,
                    "SELECT id,name,version,enabled,interval_seconds,next_run,updated_at FROM "
                    "spiders WHERE deleted=0 ORDER BY id DESC",
                    0);
            if (post) {
                json_object *rule = read_json(c, e, sizeof(e));
                if (!rule)
                    return error_json(c, 400, e);
                if (rule_validate(rule, e, sizeof(e))) {
                    json_object_put(rule);
                    return error_json(c, 400, e);
                }
                int64_t id = store_save_spider(&app->store, 0, rule, e, sizeof(e));
                json_object_put(rule);
                return id > 0 ? send_json(c, 201, id_json(id)) : error_json(c, 500, e);
            }
        }
        if (!strcmp(path, "/api/runs") && get) {
            int64_t sid = query_number(r->query_string, "spider_id", 0);
            return query_reply(
                c, app,
                "SELECT "
                "id,spider_id,version,status,pages,item_count,error,created_at,started_at,finished_"
                "at FROM runs WHERE (?1=0 OR spider_id=?1) ORDER BY id DESC LIMIT 200",
                sid);
        }
        int64_t id = 0;
        char action[80] = {0};
        int used = 0;
        if (sscanf(path, "/api/spiders/%" SCNd64 "%n", &id, &used) == 1 && id > 0) {
            const char *tail = path + used;
            if (!strcmp(tail, "/run") && post) {
                int64_t run = store_start_run(&app->store, id, e, sizeof(e));
                return run > 0 ? send_json(c, 201, id_json(run)) : error_json(c, 409, e);
            }
            if (!*tail) {
                if (get) {
                    json_object *j = one_row(app,
                                             "SELECT id,name,version,rule_json AS rule FROM "
                                             "spiders WHERE id=? AND deleted=0",
                                             id);
                    return j ? send_json(c, 200, j) : error_json(c, 404, "Spider not found");
                }
                if (put) {
                    json_object *rule = read_json(c, e, sizeof(e));
                    if (!rule)
                        return error_json(c, 400, e);
                    if (rule_validate(rule, e, sizeof(e))) {
                        json_object_put(rule);
                        return error_json(c, 400, e);
                    }
                    int64_t saved = store_save_spider(&app->store, id, rule, e, sizeof(e));
                    json_object_put(rule);
                    return saved > 0 ? send_json(c, 200, id_json(saved)) : error_json(c, 404, e);
                }
                if (del)
                    return store_delete_spider(&app->store, id, e, sizeof(e))
                               ? error_json(c, 409, e)
                               : send_json(c, 200, id_json(id));
            }
        }
        used = 0;
        if (sscanf(path, "/api/runs/%" SCNd64 "%n", &id, &used) == 1 && id > 0) {
            const char *tail = path + used;
            if (!*tail && get) {
                json_object *j = one_row(app, "SELECT * FROM runs WHERE id=?", id);
                return j ? send_json(c, 200, j) : error_json(c, 404, "Run not found");
            }
            if (!strcmp(tail, "/stop") && post)
                return store_cancel_run(&app->store, id) ? error_json(c, 409, "Run not active")
                                                         : send_json(c, 200, id_json(id));
            if (!strcmp(tail, "/items") && get) {
                int64_t after = query_number(r->query_string, "after", 0);
                char sql[256];
                snprintf(sql, sizeof(sql),
                         "SELECT id,source_url,data,created_at FROM items WHERE run_id=? AND "
                         "id>%lld ORDER BY id LIMIT 1000",
                         (long long)after);
                return query_reply(c, app, sql, id);
            }
            if (!strcmp(tail, "/steps") && get)
                return query_reply(c, app,
                                   "SELECT id,run_id,step_id,parent_id,branch_key,record_key,"
                                   "status,attempts,error,phase FROM step_instances "
                                   "WHERE run_id=? ORDER BY id DESC LIMIT 1000",
                                   id);
            if (!strncmp(tail, "/steps/", 7) && get) {
                const char *start = tail + 7;
                char *end;
                errno = 0;
                int64_t instance = strtoll(start, &end, 10);
                if (*start >= '0' && *start <= '9' && !errno && !*end && instance > 0) {
                    char sql[320];
                    snprintf(sql, sizeof(sql),
                             "SELECT id,run_id,step_id,parent_id,branch_key,record_key,"
                             "status,attempts,context,output,error,phase FROM step_instances "
                             "WHERE run_id=? AND id=%" PRId64,
                             instance);
                    json_object *j = one_row(app, sql, id);
                    if (!j)
                        return error_json(c, 404, "Step instance not found in this run");
                    json_object *run = one_row(app, "SELECT snapshot FROM runs WHERE id=?", id);
                    json_object *safe = debug_redact(j, jg(run, "snapshot"));
                    json_object_put(run);
                    json_object_put(j);
                    return send_json(c, 200, safe);
                }
                return error_json(c, 404, "Step instance not found in this run");
            }
            if (!strcmp(tail, "/events") && get)
                return query_reply(c, app,
                                   "SELECT id,level,message,created_at FROM run_events WHERE "
                                   "run_id=? ORDER BY id DESC LIMIT 200",
                                   id);
            if (!strcmp(tail, "/artifacts") && get)
                return query_reply(
                    c, app, "SELECT id,url,sha256,bytes FROM artifacts WHERE run_id=? ORDER BY id",
                    id);
            if (!strcmp(tail, "/export") && get) {
                char format[16] = {0};
                if (r->query_string)
                    mg_get_var(r->query_string, strlen(r->query_string), "format", format,
                               sizeof(format));
                return export_run(c, app, id, !strcmp(format, "csv"));
            }
        }
        if (get && sscanf(path, "/api/artifacts/%" SCNd64 "/%79s", &id, action) == 2 && id > 0 &&
            !strcmp(action, "file")) {
            json_object *j = one_row(app, "SELECT path FROM artifacts WHERE id=?", id);
            if (!j)
                return error_json(c, 404, "File not found");
            const char *file = js(j, "path", "");
            mg_send_file(c, file);
            json_object_put(j);
            return 200;
        }
        return error_json(c, 404, "API route not found");
    }
    if (get) {
        const char *file = !strcmp(path, "/")            ? "index.html"
                           : !strcmp(path, "/app.js")    ? "app.js"
                           : !strcmp(path, "/style.css") ? "style.css"
                                                         : NULL;
        if (file) {
            char filename[SP_PATH];
            if (snprintf(filename, sizeof(filename), "%s/%s", app->ui_dir, file) >=
                (int)sizeof(filename))
                return error_json(c, 500, "UI path too long");
            FILE *f = fopen(filename, "rb");
            if (!f)
                return error_json(c, 404, "UI asset not found; check --ui-dir");
            fseek(f, 0, SEEK_END);
            long len = ftell(f);
            rewind(f);
            if (len < 0 || len > 2097152) {
                fclose(f);
                return error_json(c, 500, "Invalid UI asset");
            }
            char *body = malloc((size_t)len + 1);
            if (!body) {
                fclose(f);
                return error_json(c, 500, "Out of memory");
            }
            size_t got = fread(body, 1, (size_t)len, f);
            fclose(f);
            int status = got == (size_t)len
                             ? send_bytes(c, 200,
                                          strstr(file, ".js")    ? "text/javascript; charset=utf-8"
                                          : strstr(file, ".css") ? "text/css; charset=utf-8"
                                                                 : "text/html; charset=utf-8",
                                          body, got)
                             : error_json(c, 500, "Cannot read UI asset");
            free(body);
            return status;
        }
    }
    return error_json(c, 404, "Not found");
}
static int log_message(const struct mg_connection *c, const char *message)
{
    (void)c;
    fprintf(stderr, "web: %s\n", message);
    return 1;
}
struct mg_context *web_start(App *app, char *e, size_t n)
{
    struct mg_callbacks callbacks = {0};
    callbacks.log_message = log_message;
    const char *options[] = {
        "listening_ports", app->listen,         "num_threads", "6", "request_timeout_ms",
        "15000",           "enable_keep_alive", "no",          NULL};
    mg_init_library(0);
    struct mg_context *ctx = mg_start(&callbacks, app, options);
    if (!ctx) {
        fail(e, n, "Cannot start HTTP listener on %s", app->listen);
        mg_exit_library();
        return NULL;
    }
    mg_set_request_handler(ctx, "/", handler, app);
    return ctx;
}
void web_stop(struct mg_context *ctx)
{
    if (ctx)
        mg_stop(ctx);
    mg_exit_library();
}
