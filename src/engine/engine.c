#include "engine.h"
#include "http/http.h"
#include "extract/extract.h"
#include "artifact/artifact.h"
#include "rule/rule.h"
#include <stdlib.h>
#include <string.h>
typedef struct {
    App *app;
    int64_t run;
} Execution;
static int cancelled(void *ctx)
{
    Execution *x = ctx;
    return atomic_load(&x->app->stopping) || (x->run && store_cancelled(&x->app->store, x->run));
}
static int pause_execution(Execution *x, int ms)
{
    while (ms > 0) {
        if (cancelled(x))
            return -1;
        int slice = ms > 100 ? 100 : ms;
        sleep_ms(slice);
        ms -= slice;
    }
    return cancelled(x) ? -1 : 0;
}
json_object *engine_preview(App *app, json_object *rule, char *e, size_t n)
{
    if (rule_validate(rule, e, n))
        return NULL;
    Execution ctx = {app, 0};
    HttpResponse response = {0};
    json_object *items = NULL, *out = NULL;
    char *next = NULL;
    if (http_fetch(rule, js(rule, "url", ""), NULL, cancelled, &ctx, &response, e, n))
        goto done;
    if (extract_document(rule, response.body, response.url, &items, &next, e, n))
        goto done;
    out = json_object_new_object();
    json_object_object_add(out, "items", items);
    items = NULL;
    json_object_object_add(out, "next_url", next ? json_object_new_string(next) : NULL);
    json_object *info = json_object_new_object();
    json_object_object_add(info, "status", json_object_new_int64(response.status));
    json_object_object_add(info, "url", json_object_new_string(response.url));
    json_object_object_add(info, "bytes", json_object_new_int64((int64_t)response.size));
    json_object_object_add(info, "body",
                           json_object_new_string_len(
                               response.body, response.size > 65536 ? 65536 : (int)response.size));
    json_object_object_add(info, "truncated", json_object_new_boolean(response.size > 65536));
    json_object_object_add(out, "response", info);
done:
    if (items)
        json_object_put(items);
    free(next);
    http_response_free(&response);
    return out;
}
void engine_run(App *app, json_object *run)
{
    int64_t id = json_object_get_int64(jg(run, "id"));
    json_object *rule = jg(run, "snapshot");
    Execution ctx = {app, id};
    char e[SP_ERROR] = {0};
    const char *status = "succeeded";
    int pages = ji(run, "pages", 0), max_pages = ji(rule, "max_pages", 1);
    if (rule_validate(rule, e, sizeof(e))) {
        status = "failed";
        goto done;
    }
    if (store_event(&app->store, id, "info", "Run started") ||
        store_enqueue(&app->store, id, js(rule, "url", "")) < 0) {
        fail(e, sizeof(e), "Cannot initialize persistent queue");
        status = "failed";
        goto done;
    }
    while (!cancelled(&ctx) && pages < max_pages) {
        json_object *request = store_claim_request(&app->store, id, e, sizeof(e));
        if (!request) {
            if (e[0])
                status = "failed";
            break;
        }
        int64_t request_id = json_object_get_int64(jg(request, "id"));
        const char *url = js(request, "url", "");
        HttpResponse response = {0};
        json_object *items = NULL;
        char *next = NULL;
        if (pause_execution(&ctx, ji(rule, "delay_ms", 300))) {
            json_object_put(request);
            break;
        }
        int rc = http_fetch(rule, url, NULL, cancelled, &ctx, &response, e, sizeof(e));
        if (rc) {
            http_response_free(&response);
            if (atomic_load(&app->stopping) && !store_cancelled(&app->store, id)) {
                /* Leave the claimed request running; startup requeues it. */
                json_object_put(request);
                break;
            }
            bool retry = !cancelled(&ctx) && ji(request, "attempts", 1) <= ji(rule, "retries", 2);
            if (store_request_status(&app->store, request_id, retry ? "queued" : "failed", e)) {
                fail(e, sizeof(e), "Cannot persist request retry state");
                retry = false;
            }
            store_event(&app->store, id, retry ? "warning" : "error", e);
            json_object_put(request);
            if (retry) {
                pause_execution(&ctx, 500);
                continue;
            }
            status = "failed";
            break;
        }
        rc = extract_document(rule, response.body, response.url, &items, &next, e, sizeof(e));
        if (!rc) {
            json_object *fields = jg(rule, "fields");
            for (size_t i = 0; i < json_object_array_length(items) && !rc; i++) {
                json_object *item = json_object_array_get_idx(items, i);
                for (size_t j = 0; j < json_object_array_length(fields); j++) {
                    json_object *field = json_object_array_get_idx(fields, j);
                    if (jb(field, "download", false)) {
                        json_object *v = jg(item, js(field, "name", ""));
                        if (v && json_object_is_type(v, json_type_string) &&
                            artifact_download(app, id, rule, json_object_get_string(v), cancelled,
                                              &ctx, e, sizeof(e))) {
                            rc = -1;
                            break;
                        }
                    }
                }
            }
        }
        if (!rc && cancelled(&ctx))
            rc = fail(e, sizeof(e), "Cancelled");
        if (!rc && store_commit_page(&app->store, id, request_id, response.url, items,
                                     pages + 1 < max_pages ? next : NULL))
            rc = fail(e, sizeof(e), "Cannot commit extracted data to SQLite");
        if (rc) {
            if (!atomic_load(&app->stopping) || store_cancelled(&app->store, id))
                store_request_status(&app->store, request_id, "failed", e);
            status = "failed";
        } else {
            pages++;
            e[0] = 0;
            store_event(&app->store, id, "info", "Page parsed and committed");
        }
        free(next);
        if (items)
            json_object_put(items);
        http_response_free(&response);
        json_object_put(request);
        if (rc)
            break;
    }
done:
    /* Shutdown preserves running work for recovery; explicit stop is terminal. */
    if (atomic_load(&app->stopping) && !store_cancelled(&app->store, id)) {
        store_event(&app->store, id, "info", "Service shutdown; run will resume on restart");
        return;
    }
    if (store_cancelled(&app->store, id))
        status = "cancelled";
    if (store_finish_run(&app->store, id, status, e))
        fprintf(stderr, "Cannot finish run %lld\n", (long long)id);
    store_event(&app->store, id, !strcmp(status, "failed") ? "error" : "info",
                !strcmp(status, "failed") ? e : status);
}
