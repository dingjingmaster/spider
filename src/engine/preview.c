#include "preview.h"
#include "workflow.h"
#include "common/redact.h"
#include "rule/rule.h"
#include "rule/workflow.h"
#include "rule/data.h"
#include "http/http.h"
#include <openssl/rand.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PREVIEW_CAPACITY 16
#define PREVIEW_TTL 600000
#define PREVIEW_STEPS 256
#define PREVIEW_BYTES (4U * 1024U * 1024U)

typedef struct PreviewSession {
    char id[65];
    json_object *rule, *context, *iterations;
    HttpSession *http;
    pthread_mutex_t mutex;
    unsigned refs, steps;
    atomic_bool released;
    int64_t used;
    struct PreviewSession *next;
} PreviewSession;
struct PreviewPool {
    pthread_mutex_t mutex;
    PreviewSession *head;
};
static int64_t timestamp(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static void destroy(PreviewSession *s)
{
    json_object_put(s->rule);
    json_object_put(s->context);
    json_object_put(s->iterations);
    session_destroy(s->http);
    pthread_mutex_destroy(&s->mutex);
    free(s);
}
PreviewPool *preview_pool_create(void)
{
    PreviewPool *p = calloc(1, sizeof(*p));
    if (p)
        pthread_mutex_init(&p->mutex, NULL);
    return p;
}
void preview_pool_destroy(PreviewPool *p)
{
    if (!p)
        return;
    PreviewSession *s = p->head;
    while (s) {
        PreviewSession *next = s->next;
        destroy(s);
        s = next;
    }
    pthread_mutex_destroy(&p->mutex);
    free(p);
}
int preview_pool_release(PreviewPool *p, const char *id, char *e, size_t n)
{
    if (!p || !id || strlen(id) != 64 || strspn(id, "0123456789abcdef") != 64)
        return fail(e, n, "Invalid preview session_id");
    pthread_mutex_lock(&p->mutex);
    PreviewSession **link = &p->head;
    while (*link && strcmp((*link)->id, id))
        link = &(*link)->next;
    PreviewSession *s = *link;
    int rc = -1;
    if (s && !atomic_load(&s->released)) {
        atomic_store(&s->released, true);
        if (!s->refs) {
            *link = s->next;
            destroy(s);
        }
        rc = 0;
    }
    pthread_mutex_unlock(&p->mutex);
    return rc ? fail(e, n, "Preview session is invalid, expired or already released") : 0;
}
static int bounded(json_object *value, char *e, size_t n)
{
    size_t bytes = 0;
    json_object_to_json_string_length(value, JSON_C_TO_STRING_PLAIN, &bytes);
    return bytes > PREVIEW_BYTES ? fail(e, n, "Preview context/output exceeds 4 MiB") : 0;
}
static PreviewSession *acquire(PreviewPool *p, json_object *rule, json_object *payload,
                               bool *created, char *e, size_t n)
{
    const char *id = js(payload, "session_id", "");
    json_object *v = NULL;
    *created = false;
    if (json_object_object_get_ex(payload, "session_id", &v) &&
        (!json_object_is_type(v, json_type_string) || strlen(id) != 64 ||
         strspn(id, "0123456789abcdef") != 64)) {
        fail(e, n, "Invalid preview session_id");
        return NULL;
    }
    if (*id && json_object_object_get_ex(payload, "context", &v)) {
        fail(e, n, "context is accepted only when creating a preview session");
        return NULL;
    }
    if (!*id && json_object_object_get_ex(payload, "context", &v) &&
        (!json_object_is_type(v, json_type_object) || bounded(v, e, n))) {
        if (!e || !*e)
            fail(e, n, "Preview context must be an object");
        return NULL;
    }
    pthread_mutex_lock(&p->mutex);
    int64_t now = timestamp();
    unsigned count = 0;
    PreviewSession **link = &p->head, *s = NULL;
    while (*link) {
        PreviewSession *cur = *link;
        if (!cur->refs && now - cur->used >= PREVIEW_TTL) {
            *link = cur->next;
            destroy(cur);
            continue;
        }
        if (*id && !strcmp(cur->id, id) && !atomic_load(&cur->released))
            s = cur;
        count++;
        link = &cur->next;
    }
    if (*id && !s)
        fail(e, n, "Preview session is invalid or expired; start a new session");
    else if (!*id && count >= PREVIEW_CAPACITY)
        fail(e, n, "Preview session limit (16) reached; wait for idle sessions to expire");
    else if (!*id) {
        s = calloc(1, sizeof(*s));
        if (s) {
            pthread_mutex_init(&s->mutex, NULL);
            atomic_init(&s->released, false);
            unsigned char random[32];
            s->rule = flow_clone(rule);
            s->context = workflow_context(rule, jg(payload, "context"));
            s->iterations = json_object_new_object();
            s->http = session_temporary();
            if (!s->rule || !s->context || !s->iterations || !s->http ||
                RAND_bytes(random, sizeof(random)) != 1 || bounded(s->context, e, n)) {
                destroy(s);
                s = NULL;
                if (!e || !*e)
                    fail(e, n, "Cannot allocate preview session");
            } else {
                for (size_t i = 0; i < sizeof(random); i++)
                    snprintf(s->id + 2 * i, 3, "%02x", random[i]);
                s->next = p->head;
                p->head = s;
                *created = true;
            }
        } else
            fail(e, n, "Cannot allocate preview session");
    }
    if (s)
        s->refs++;
    pthread_mutex_unlock(&p->mutex);
    if (s)
        pthread_mutex_lock(&s->mutex);
    return s;
}
static void release(PreviewPool *p, PreviewSession *s, bool discard)
{
    pthread_mutex_unlock(&s->mutex);
    pthread_mutex_lock(&p->mutex);
    s->refs--;
    s->used = timestamp();
    if ((discard || atomic_load(&s->released)) && !s->refs) {
        PreviewSession **link = &p->head;
        while (*link && *link != s)
            link = &(*link)->next;
        if (*link) {
            *link = s->next;
            destroy(s);
        }
    }
    pthread_mutex_unlock(&p->mutex);
}
/* Work on an isolated Cookie jar so failed requests do not advance session state. */
static HttpSession *copy_http(HttpSession *source)
{
    CURL *read = curl_easy_init(), *write = curl_easy_init();
    HttpSession *copy = session_temporary();
    struct curl_slist *cookies = NULL;
    bool ok = read && write && copy;
    if (ok) {
        session_attach(source, read);
        session_attach(copy, write);
        ok = curl_easy_getinfo(read, CURLINFO_COOKIELIST, &cookies) == CURLE_OK;
        size_t bytes = 0;
        for (struct curl_slist *c = cookies; ok && c; c = c->next) {
            bytes += strlen(c->data);
            ok = bytes <= 1048576 &&
                 curl_easy_setopt(write, CURLOPT_COOKIELIST, c->data) == CURLE_OK;
        }
    }
    curl_slist_free_all(cookies);
    curl_easy_cleanup(read);
    curl_easy_cleanup(write);
    if (!ok) {
        session_destroy(copy);
        copy = NULL;
    }
    return copy;
}
static int stopping(void *context)
{
    return atomic_load(&((App *)context)->stopping);
}
json_object *workflow_preview_step(App *app, json_object *payload, char *e, size_t n)
{
    if (e && n)
        *e = 0;
    if (!payload || !json_object_is_type(payload, json_type_object) || !app->previews) {
        fail(e, n, "Preview requires an object and an available session pool");
        return NULL;
    }
    json_object *rule = jg(payload, "rule"), *dry_value = NULL;
    if (rule_validate(rule, e, n))
        return NULL;
    if (json_object_object_get_ex(payload, "dry_run", &dry_value) &&
        !json_object_is_type(dry_value, json_type_boolean)) {
        fail(e, n, "dry_run must be boolean");
        return NULL;
    }
    const char *id = js(payload, "step_id", "");
    json_object *step = flow_step(rule, id);
    if (!step) {
        fail(e, n, "Unknown preview step_id");
        return NULL;
    }
    const char *type = js(step, "type", "");
    if (!strcmp(type, "download")) {
        fail(e, n, "Download steps are not executed in preview");
        return NULL;
    }
    bool created = false, dry = jb(payload, "dry_run", false);
    PreviewSession *s = acquire(app->previews, rule, payload, &created, e, n);
    if (!s)
        return NULL;
    json_object *ctx = NULL, *out = NULL, *request = NULL, *reply = NULL, *clean = NULL;
    HttpSession *http = NULL;
    HttpResponse response = {0};
    const char *next = js(step, "next", ""), *notice = "";
    int iteration = 0;
    if (atomic_load(&s->released)) {
        fail(e, n, "Preview session was released; start a new session");
        goto done;
    }
    if (!json_object_equal(s->rule, rule)) {
        fail(e, n, "Rule changed; start a new preview session");
        goto done;
    }
    if (s->steps >= PREVIEW_STEPS) {
        fail(e, n, "Preview step limit (256) reached; start a new session");
        goto done;
    }
    ctx = flow_clone(s->context);
    if (!ctx || debug_track_secrets(ctx, NULL, rule, e, n) || flow_input_prepare(step, ctx, e, n))
        goto done;
    if (!strcmp(type, "request")) {
        request = workflow_request(rule, step, ctx, e, n);
        if (!request)
            goto done;
        if (debug_track_secrets(ctx, request, rule, e, n))
            goto done;
        if (!dry) {
            http = copy_http(s->http);
            if (!http) {
                fail(e, n, "Cannot copy preview Cookie session");
                goto done;
            }
            if (http_fetch_session(request, js(request, "url", ""), NULL, stopping, app, http,
                                   &response, e, n))
                goto done;
            out = workflow_response(step, request, &response, e, n);
            if (!out)
                goto done;
            iteration = ji(s->iterations, id, 0) + 1;
            json_object_object_add(out, "iteration", json_object_new_int(iteration));
            json_object_object_add(ctx, "source_url", json_object_new_string(response.url));
        } else
            notice = "Dry run: request resolved without sending or advancing the session";
    } else if (!strcmp(type, "transform")) {
        if (flow_transform(step, ctx, &out, e, n) ||
            debug_track_transform(ctx, step, out, rule, e, n))
            goto done;
    } else if (!strcmp(type, "join") || !strcmp(type, "emit")) {
        if (flow_render(jg(step, "fields"), ctx, &out, e, n))
            goto done;
        if (!strcmp(type, "emit"))
            notice = "Preview only: emitted data is not saved";
    } else if (!strcmp(type, "condition")) {
        bool result = false;
        if (flow_test(jg(step, "test"), ctx, &result, e, n))
            goto done;
        out = json_object_new_object();
        json_object_object_add(out, "result", json_object_new_boolean(result));
        next = js(step, result ? "then" : "else", "");
    } else if (!strcmp(type, "foreach")) {
        json_object *items = NULL, *index = NULL;
        if (flow_render(jg(step, "items"), ctx, &items, e, n))
            goto done;
        bool supplied = json_object_object_get_ex(payload, "item_index", &index);
        int64_t selected = supplied ? json_object_get_int64(index) : 0;
        if (!json_object_is_type(items, json_type_array) ||
            json_object_array_length(items) > 1000 ||
            (supplied && !json_object_is_type(index, json_type_int)) || selected < 0 ||
            (uint64_t)selected >= json_object_array_length(items)) {
            json_object_put(items);
            fail(e, n, "foreach requires a nonempty array (max 1000) and a valid item_index");
            goto done;
        }
        out = flow_clone(json_object_array_get_idx(items, (size_t)selected));
        json_object_object_add(ctx, "input", flow_clone(out));
        json_object_put(items);
        next = js(step, "body", "");
        notice = supplied ? "Only the selected foreach item is previewed"
                          : "No item_index supplied: previewing only the first foreach item";
    } else if (!strcmp(type, "parallel")) {
        const char *branch = js(payload, "branch", "");
        json_object *target = *branch ? jg(jg(step, "branches"), branch) : NULL;
        if (!target || !json_object_is_type(target, json_type_string)) {
            fail(e, n, "parallel preview requires a valid branch selection");
            goto done;
        }
        next = json_object_get_string(target);
        out = json_object_new_object();
        json_object_object_add(out, "branch", json_object_new_string(branch));
        notice = "Only the selected parallel branch is previewed";
    } else {
        fail(e, n, "Unsupported preview step type");
        goto done;
    }
    if (!(dry && !strcmp(type, "request")) && workflow_complete_context(rule, step, ctx, out, e, n))
        goto done;
    if (bounded(ctx, e, n) || bounded(out, e, n))
        goto done;
    /* Sources are an array to avoid treating transport wrapper names as
     * site-defined sensitive fields. Context keeps bounded secret provenance. */
    reply = json_object_new_array();
    json_object_array_add(reply, json_object_get(ctx));
    json_object_array_add(reply, json_object_get(out));
    json_object_array_add(reply, json_object_get(request));
    clean = json_object_new_object();
    if (!clean) {
        fail(e, n, "Cannot allocate preview response");
        goto done;
    }
    json_object_object_add(clean, "output", debug_redact_with_context(out, rule, reply));
    json_object_object_add(clean, "context",
                           debug_redact_with_context(dry ? s->context : ctx, rule, reply));
    json_object_object_add(clean, "request", debug_redact_with_context(request, rule, reply));
    /* Transport fields are required to continue the preview, even if a site's
     * data uses identically named custom sensitive fields. */
    json_object_object_add(clean, "session_id", json_object_new_string(s->id));
    json_object_object_add(clean, "step_id", json_object_new_string(id));
    json_object *next_steps = json_object_new_array();
    if (*next)
        json_object_array_add(next_steps, json_object_new_string(next));
    json_object_object_add(clean, "next_steps", next_steps);
    json_object_object_add(clean, "notice", json_object_new_string(notice));
    if (!dry) {
        json_object_put(s->context);
        s->context = ctx;
        ctx = NULL;
        s->steps++;
        if (iteration)
            json_object_object_add(s->iterations, id, json_object_new_int(iteration));
        if (http) {
            session_destroy(s->http);
            s->http = http;
            http = NULL;
        }
    }
done:
    json_object_put(reply);
    json_object_put(request);
    json_object_put(out);
    json_object_put(ctx);
    http_response_free(&response);
    session_destroy(http);
    release(app->previews, s, created && !clean);
    return clean;
}
