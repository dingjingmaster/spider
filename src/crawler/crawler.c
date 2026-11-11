#include "crawler.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define CRAWLER_CAPACITY 64
struct CrawlerRegistry {
    CrawlerDefinition definitions[CRAWLER_CAPACITY];
    size_t count;
    bool frozen;
};
static const CrawlerDefinition *find(CrawlerRegistry *r, const char *id)
{
    if (r)
        for (size_t i = 0; i < r->count; i++)
            if (!strcmp(r->definitions[i].id, id))
                return &r->definitions[i];
    return NULL;
}
CrawlerRegistry *crawler_create(void)
{
    return calloc(1, sizeof(CrawlerRegistry));
}
void crawler_destroy(CrawlerRegistry *r)
{
    if (!r)
        return;
    for (size_t i = 0; i < r->count; i++) {
        CrawlerDefinition *d = &r->definitions[i];
        if (d->destroy)
            d->destroy(d->userdata);
        free((char *)d->id);
        free((char *)d->name);
    }
    free(r);
}
int crawler_register(CrawlerRegistry *r, const CrawlerDefinition *d, char *e, size_t n)
{
    if (!r || !d || !d->id || !*d->id || strlen(d->id) > 63 ||
        strspn(d->id, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") !=
            strlen(d->id) ||
        !d->name || !d->run || r->frozen || r->count == CRAWLER_CAPACITY || find(r, d->id))
        return fail(e, n, "Invalid, duplicate or frozen crawler registration");
    char *id = strdup(d->id), *name = strdup(d->name);
    if (!id || !name) {
        free(id);
        free(name);
        return fail(e, n, "Cannot copy crawler definition");
    }
    r->definitions[r->count] = *d;
    r->definitions[r->count].id = id;
    r->definitions[r->count].name = name;
    r->count++;
    return 0;
}
void crawler_freeze(CrawlerRegistry *r)
{
    if (r)
        r->frozen = true;
}
json_object *crawler_definitions(CrawlerRegistry *r)
{
    json_object *list = json_object_new_array();
    for (size_t i = 0; r && i < r->count; i++) {
        json_object *row = json_object_new_object();
        json_object_object_add(row, "id", json_object_new_string(r->definitions[i].id));
        json_object_object_add(row, "name", json_object_new_string(r->definitions[i].name));
        json_object_array_add(list, row);
    }
    return list;
}
int crawler_validate(CrawlerRegistry *r, json_object *config, char *e, size_t n)
{
    if (!json_object_is_type(config, json_type_object))
        return fail(e, n, "Crawler config must be an object");
    json_object *name = jg(config, "name"), *kind = jg(config, "implementation"),
                *interval = jg(config, "interval_seconds"), *enabled = jg(config, "enabled");
    if (!json_object_is_type(name, json_type_string) || !*json_object_get_string(name) ||
        json_object_get_string_len(name) > 160 ||
        strlen(json_object_get_string(name)) != (size_t)json_object_get_string_len(name))
        return fail(e, n, "name must contain 1-160 bytes");
    if (kind && (!json_object_is_type(kind, json_type_string) ||
                 strlen(json_object_get_string(kind)) != (size_t)json_object_get_string_len(kind)))
        return fail(e, n, "Invalid crawler implementation");
    if (interval &&
        (!json_object_is_type(interval, json_type_int) || json_object_get_int64(interval) < 0 ||
         json_object_get_int64(interval) > 31536000))
        return fail(e, n, "Invalid interval_seconds");
    if (enabled && !json_object_is_type(enabled, json_type_boolean))
        return fail(e, n, "enabled must be boolean");
    const CrawlerDefinition *d = find(r, js(config, "implementation", "workflow"));
    if (!d)
        return fail(e, n, "Unknown crawler implementation");
    return d->validate ? d->validate(config, e, n) : 0;
}
int crawler_cancelled(void *context)
{
    CrawlerContext *c = context;
    return !c || (c->stopping && atomic_load(c->stopping)) ||
           (c->run_id && !store_run_active(c->store, c->run_id));
}
static void progress(void *context, int64_t received, int64_t total)
{
    CrawlerContext *c = context;
    status_progress(c->status, c->run_id, NULL, received, total);
}
static int persist_session(CrawlerContext *c, char *e, size_t n)
{
    json_object *cookies = download_session_export(c->session, e, n);
    if (!cookies)
        return -1;
    int rc = store_session_save(c->store, c->run_id, cookies, e, n);
    json_object_put(cookies);
    return rc;
}
int crawler_execute(CrawlerRegistry *r, CrawlerContext *c, char *e, size_t n)
{
    if (crawler_validate(r, c->config, e, n))
        return -1;
    const CrawlerDefinition *d = find(r, js(c->config, "implementation", "workflow"));
    json_object *cookies = store_session_load(c->store, c->run_id, e, n);
    if (!cookies)
        return -1;
    c->session = download_session_create(cookies, e, n);
    json_object_put(cookies);
    if (!c->session)
        return -1;
    download_session_set_progress(c->session, progress, c);
    int rc = d->run(c, e, n);
    /* Save unfinished native calls on graceful shutdown as well. */
    if (store_run_active(c->store, c->run_id) && persist_session(c, e, n))
        rc = -1;
    download_session_destroy(c->session);
    c->session = NULL;
    return rc;
}
int crawler_fetch(CrawlerContext *c, json_object *options, const char *url, HttpResponse *response,
                  char *e, size_t n)
{
    if (crawler_cancelled(c))
        return fail(e, n, "Cancelled");
    int rc = download_fetch(options, url, NULL, crawler_cancelled, c, c->session, response, e, n);
    char saved[SP_ERROR] = {0};
    if (persist_session(c, saved, sizeof(saved)))
        return fail(e, n, "%s", saved);
    return rc;
}
static int64_t record(void *ctx, const char *url, const char *path, const char *hash, int64_t bytes)
{
    CrawlerContext *c = ctx;
    return store_add_artifact(c->store, c->run_id, url, path, hash, bytes);
}
int crawler_save_file(CrawlerContext *c, json_object *options, const char *url, DownloadFile *file,
                      char *e, size_t n)
{
    if (crawler_cancelled(c))
        return fail(e, n, "Cancelled");
    json_object *rows = store_artifacts(c->store, c->run_id, e, n);
    if (!rows)
        return -1;
    for (size_t i = 0; i < json_object_array_length(rows); i++) {
        json_object *a = json_object_array_get_idx(rows, i);
        if (!strcmp(js(a, "url", ""), url) && access(js(a, "path", ""), R_OK) == 0) {
            memset(file, 0, sizeof(*file));
            file->id = json_object_get_int64(jg(a, "id"));
            file->bytes = json_object_get_int64(jg(a, "bytes"));
            snprintf(file->path, sizeof(file->path), "%s", js(a, "path", ""));
            snprintf(file->sha256, sizeof(file->sha256), "%s", js(a, "sha256", ""));
            json_object_put(rows);
            return 0;
        }
    }
    json_object_put(rows);
    DownloadRequest request = {.group_id = c->run_id,
                               .options = options,
                               .url = url,
                               .session = c->session,
                               .cancelled = crawler_cancelled,
                               .cancel_context = c,
                               .record = record,
                               .record_context = c};
    int rc = download_save(c->downloads, &request, file, e, n);
    char saved[SP_ERROR] = {0};
    if (persist_session(c, saved, sizeof(saved)))
        return fail(e, n, "%s", saved);
    return rc;
}
json_object *crawler_preview(CrawlerRegistry *r, json_object *payload, bool single,
                             const atomic_bool *stopping, char *e, size_t n)
{
    json_object *config = jg(payload, "rule");
    if (!config)
        config = payload;
    if (crawler_validate(r, config, e, n))
        return NULL;
    const CrawlerDefinition *d = find(r, js(config, "implementation", "workflow"));
    if (!d->preview) {
        fail(e, n, "Crawler does not support preview");
        return NULL;
    }
    return d->preview(d->userdata, payload, single, stopping, e, n);
}
int crawler_release_preview(CrawlerRegistry *r, const char *kind, const char *id, char *e, size_t n)
{
    const CrawlerDefinition *d = find(r, kind ? kind : "workflow");
    return d && d->release_preview ? d->release_preview(d->userdata, id, e, n)
                                   : fail(e, n, "Crawler does not support preview sessions");
}
