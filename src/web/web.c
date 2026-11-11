#include "web.h"
#include <civetweb.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
struct Web {
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
static bool token_equal(const char *a, const char *b)
{
    size_t al = strlen(a), bl = strlen(b);
    unsigned diff = (unsigned)(al ^ bl);
    for (size_t i = 0; i < bl; i++)
        diff |= (unsigned char)(i < al ? a[i] : 0) ^ (unsigned char)b[i];
    return diff == 0;
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
    const char *path = r->local_uri;
    bool get = !strcmp(r->request_method, "GET");
    char e[SP_ERROR] = {0};
    if (!strncmp(path, "/api/", 5)) {
        if (app->token[0]) {
            const char *auth = mg_get_header(c, "Authorization");
            if (!auth || strncmp(auth, "Bearer ", 7) || !token_equal(auth + 7, app->token))
                return error_json(c, 401, "API token required");
        }
        if (!get)
            return error_json(c, 405, "Status API is read-only");
        if (!strcmp(path, "/api/health")) {
            json_object *row = json_object_new_object();
            json_object_object_add(row, "version", json_object_new_string("0.1.0"));
            json_object_object_add(row, "read_only", json_object_new_boolean(true));
            return send_json(c, 200, row);
        }
        if (!strcmp(path, "/api/spiders")) {
            json_object *rows = status_spiders(app->status, e, sizeof(e));
            return reply_rows(c, rows, e);
        }
        if (!strcmp(path, "/api/runs")) {
            int64_t spider = query_number(r->query_string, "spider_id", 0);
            if (spider) {
                json_object *row = status_spider(app->status, spider);
                if (!row)
                    return error_json(c, 404, "Code crawler not found");
                json_object_put(row);
            }
            json_object *rows = status_history(app->status, spider, e, sizeof(e));
            return reply_rows(c, rows, e);
        }
        int64_t id = 0;
        int used = 0;
        if (sscanf(path, "/api/spiders/%" SCNd64 "%n", &id, &used) == 1 && id > 0 && !path[used]) {
            json_object *row = status_spider(app->status, id);
            return row ? send_json(c, 200, row) : error_json(c, 404, "Code crawler not found");
        }
        used = 0;
        if (sscanf(path, "/api/runs/%" SCNd64 "%n", &id, &used) == 1 && id > 0) {
            const char *tail = path + used;
            if (*tail && strcmp(tail, "/events"))
                return error_json(c, 404, "Status route not found");
            json_object *run = status_code_run(app->status, id);
            if (!run)
                return error_json(c, 404, "Code crawler run not found");
            if (!*tail)
                return send_json(c, 200, run);
            json_object_put(run);
            json_object *rows = status_events(app->status, id, e, sizeof(e));
            return reply_rows(c, rows, e);
        }
        return error_json(c, 404, "Status route not found");
    }
    if (!get)
        return error_json(c, 405, "Status page is read-only");
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
Web *web_start(const WebConfig *config, Status *status, char *e, size_t n)
{
    if (!config || !config->listen || !config->ui_directory || !status) {
        fail(e, n, "Invalid Web dependencies");
        return NULL;
    }
    Web *web = calloc(1, sizeof(*web));
    if (!web) {
        fail(e, n, "Cannot allocate Web server");
        return NULL;
    }
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
