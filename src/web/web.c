#include "web.h"
#include "common/redact.h"
#include <civetweb.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
struct Web {
    Store *store;
    Scheduler *scheduler;
    CrawlerRegistry *registry;
    Status *status;
    struct mg_context *server;
    char listen[128], ui_dir[SP_PATH], token[256];
};
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
static int reply_rows(struct mg_connection *c, json_object *rows, const char *error)
{
    return rows ? send_json(c, 200, rows)
                : error_json(c, 500, error && *error ? error : "Cannot query data");
}
static int64_t query_number(const char *query, const char *key, int64_t fallback)
{
    char value[40] = {0};
    if (!query || mg_get_var(query, strlen(query), key, value, sizeof(value)) < 0)
        return fallback;
    char *end;
    errno = 0;
    long long result = strtoll(value, &end, 10);
    return errno || *end || result < 0 ? fallback : (int64_t)result;
}
static int handler(struct mg_connection *c, void *context)
{
    Web *app = context;
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
        if (get && !strcmp(path, "/api/implementations"))
            return send_json(c, 200, crawler_definitions(app->registry));
        if (post && !strcmp(path, "/api/preview/release")) {
            json_object *payload = read_json(c, e, sizeof(e));
            if (!payload)
                return error_json(c, 400, e);
            int rc =
                crawler_release_preview(app->registry, js(payload, "implementation", "workflow"),
                                        js(payload, "session_id", ""), e, sizeof(e));
            json_object_put(payload);
            if (rc)
                return error_json(c, 404, e);
            json_object *reply = json_object_new_object();
            json_object_object_add(reply, "released", json_object_new_boolean(true));
            return send_json(c, 200, reply);
        }
        if (post && (!strcmp(path, "/api/preview") || !strcmp(path, "/api/preview/step"))) {
            json_object *payload = read_json(c, e, sizeof(e));
            if (!payload)
                return error_json(c, 400, e);
            json_object *definition = jg(payload, "rule");
            if (crawler_validate(app->registry, definition ? definition : payload, e, sizeof(e))) {
                json_object_put(payload);
                return error_json(c, 400, e);
            }
            json_object *out =
                crawler_preview(app->registry, payload, !strcmp(path, "/api/preview/step"),
                                scheduler_stopping(app->scheduler), e, sizeof(e));
            json_object_put(payload);
            return out ? send_json(c, 200, out) : error_json(c, 422, e);
        }
        if (!strcmp(path, "/api/spiders")) {
            if (get) {
                json_object *rows = store_spiders(app->store, e, sizeof(e));
                return reply_rows(c, rows, e);
            }
            if (post) {
                json_object *config = read_json(c, e, sizeof(e));
                if (!config)
                    return error_json(c, 400, e);
                if (crawler_validate(app->registry, config, e, sizeof(e))) {
                    json_object_put(config);
                    return error_json(c, 400, e);
                }
                int64_t id = store_save_spider(app->store, 0, config, e, sizeof(e));
                json_object_put(config);
                return id > 0 ? send_json(c, 201, id_json(id)) : error_json(c, 500, e);
            }
        }
        if (get && !strcmp(path, "/api/runs")) {
            json_object *rows = status_runs(
                app->status, query_number(r->query_string, "spider_id", 0), e, sizeof(e));
            return reply_rows(c, rows, e);
        }
        int64_t id = 0;
        char action[80] = {0};
        int used = 0;
        if (sscanf(path, "/api/spiders/%" SCNd64 "%n", &id, &used) == 1 && id > 0) {
            const char *tail = path + used;
            if (post && !strcmp(tail, "/run")) {
                int64_t run = scheduler_submit(app->scheduler, id, e, sizeof(e));
                return run > 0 ? send_json(c, 201, id_json(run)) : error_json(c, 409, e);
            }
            if (!*tail) {
                if (get) {
                    json_object *row = store_spider(app->store, id);
                    return row ? send_json(c, 200, row) : error_json(c, 404, "Spider not found");
                }
                if (put) {
                    json_object *config = read_json(c, e, sizeof(e));
                    if (!config)
                        return error_json(c, 400, e);
                    if (crawler_validate(app->registry, config, e, sizeof(e))) {
                        json_object_put(config);
                        return error_json(c, 400, e);
                    }
                    int64_t saved = store_save_spider(app->store, id, config, e, sizeof(e));
                    json_object_put(config);
                    return saved > 0 ? send_json(c, 200, id_json(saved)) : error_json(c, 404, e);
                }
                if (del)
                    return store_delete_spider(app->store, id, e, sizeof(e))
                               ? error_json(c, 409, e)
                               : send_json(c, 200, id_json(id));
            }
        }
        used = 0;
        if (sscanf(path, "/api/runs/%" SCNd64 "%n", &id, &used) == 1 && id > 0) {
            const char *tail = path + used;
            if (!*tail && get) {
                json_object *row = status_run(app->status, id);
                return row ? send_json(c, 200, row) : error_json(c, 404, "Run not found");
            }
            if (post && !strcmp(tail, "/stop"))
                return scheduler_cancel(app->scheduler, id, e, sizeof(e))
                           ? error_json(c, 409, e)
                           : send_json(c, 200, id_json(id));
            if (get && !strcmp(tail, "/items")) {
                json_object *rows = store_items(
                    app->store, id, query_number(r->query_string, "after", 0), e, sizeof(e));
                return reply_rows(c, rows, e);
            }
            if (get && !strcmp(tail, "/steps")) {
                json_object *rows = status_steps(app->status, id, e, sizeof(e));
                return reply_rows(c, rows, e);
            }
            if (get && !strncmp(tail, "/steps/", 7)) {
                const char *start = tail + 7;
                char *end;
                errno = 0;
                int64_t instance = strtoll(start, &end, 10);
                if (*start >= '0' && *start <= '9' && !errno && !*end && instance > 0) {
                    json_object *row = status_step(app->status, id, instance);
                    if (!row)
                        return error_json(c, 404, "Step instance not found in this run");
                    json_object *run = store_run(app->store, id);
                    json_object *safe = debug_redact(row, jg(run, "snapshot"));
                    json_object_put(run);
                    json_object_put(row);
                    return send_json(c, 200, safe);
                }
                return error_json(c, 404, "Step instance not found in this run");
            }
            if (get && !strcmp(tail, "/events")) {
                json_object *rows = status_events(app->status, id, e, sizeof(e));
                return reply_rows(c, rows, e);
            }
            if (get && !strcmp(tail, "/artifacts")) {
                json_object *rows = store_artifacts(app->store, id, e, sizeof(e));
                for (size_t i = 0; rows && i < json_object_array_length(rows); i++)
                    json_object_object_del(json_object_array_get_idx(rows, i), "path");
                return reply_rows(c, rows, e);
            }
            if (get && !strcmp(tail, "/export")) {
                char format[16] = {0};
                if (r->query_string)
                    mg_get_var(r->query_string, strlen(r->query_string), "format", format,
                               sizeof(format));
                bool csv = !strcmp(format, "csv");
                char *body = NULL;
                size_t size = 0;
                int rc = store_export(app->store, id, csv, &body, &size, e, sizeof(e));
                if (rc)
                    return error_json(c, rc, e);
                rc = send_bytes(c, 200,
                                csv ? "text/csv; charset=utf-8" : "application/json; charset=utf-8",
                                body, size);
                free(body);
                return rc;
            }
        }
        if (get && sscanf(path, "/api/artifacts/%" SCNd64 "/%79s", &id, action) == 2 && id > 0 &&
            !strcmp(action, "file")) {
            json_object *row = store_artifact(app->store, id);
            if (!row)
                return error_json(c, 404, "File not found");
            mg_send_file(c, js(row, "path", ""));
            json_object_put(row);
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
Web *web_start(const WebConfig *config, Store *store, Scheduler *scheduler,
               CrawlerRegistry *registry, Status *status, char *e, size_t n)
{
    if (!config || !config->listen || !config->ui_directory || !store || !scheduler || !registry ||
        !status) {
        fail(e, n, "Invalid Web dependencies");
        return NULL;
    }
    Web *web = calloc(1, sizeof(*web));
    if (!web) {
        fail(e, n, "Cannot allocate Web server");
        return NULL;
    }
    web->store = store;
    web->scheduler = scheduler;
    web->registry = registry;
    web->status = status;
    if (strlen(config->listen) >= sizeof(web->listen) ||
        strlen(config->ui_directory) >= sizeof(web->ui_dir) ||
        (config->token && strlen(config->token) >= sizeof(web->token))) {
        free(web);
        fail(e, n, "Web configuration too long");
        return NULL;
    }
    strcpy(web->listen, config->listen);
    strcpy(web->ui_dir, config->ui_directory);
    if (config->token)
        strcpy(web->token, config->token);
    struct mg_callbacks callbacks = {0};
    callbacks.log_message = log_message;
    const char *options[] = {
        "listening_ports", web->listen,         "num_threads", "6", "request_timeout_ms",
        "15000",           "enable_keep_alive", "no",          NULL};
    mg_init_library(0);
    web->server = mg_start(&callbacks, web, options);
    if (!web->server) {
        fail(e, n, "Cannot start HTTP listener on %s", web->listen);
        mg_exit_library();
        free(web);
        return NULL;
    }
    mg_set_request_handler(web->server, "/", handler, web);
    return web;
}
void web_stop(Web *web)
{
    if (web) {
        mg_stop(web->server);
        mg_exit_library();
        free(web);
    }
}
