#include "status.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct Progress {
    int64_t run, received, total;
    char phase[1024];
    struct Progress *next;
} Progress;
struct Status {
    Store *store;
    pthread_mutex_t mutex;
    Progress *head;
};
Status *status_create(Store *store)
{
    Status *s = calloc(1, sizeof(*s));
    if (s) {
        s->store = store;
        pthread_mutex_init(&s->mutex, NULL);
    }
    return s;
}
void status_destroy(Status *s)
{
    if (!s)
        return;
    while (s->head) {
        Progress *p = s->head;
        s->head = p->next;
        free(p);
    }
    pthread_mutex_destroy(&s->mutex);
    free(s);
}
void status_progress(Status *s, int64_t run, const char *phase, int64_t received, int64_t total)
{
    if (!s)
        return;
    pthread_mutex_lock(&s->mutex);
    Progress *p = s->head;
    while (p && p->run != run)
        p = p->next;
    if (!p) {
        p = calloc(1, sizeof(*p));
        if (p) {
            p->run = run;
            p->next = s->head;
            s->head = p;
        }
    }
    if (p) {
        if (phase)
            snprintf(p->phase, sizeof(p->phase), "%s", phase);
        p->received = received;
        p->total = total;
    }
    pthread_mutex_unlock(&s->mutex);
}
void status_forget(Status *s, int64_t run)
{
    if (!s)
        return;
    pthread_mutex_lock(&s->mutex);
    Progress **link = &s->head;
    while (*link && (*link)->run != run)
        link = &(*link)->next;
    if (*link) {
        Progress *p = *link;
        *link = p->next;
        free(p);
    }
    pthread_mutex_unlock(&s->mutex);
}
json_object *status_run(Status *s, int64_t run)
{
    json_object *row = store_run(s->store, run);
    if (!row)
        return NULL;
    bool executing = false;
    pthread_mutex_lock(&s->mutex);
    for (Progress *p = s->head; p; p = p->next)
        if (p->run == run) {
            executing = true;
            json_object *progress = json_object_new_object();
            json_object_object_add(progress, "phase", json_object_new_string(p->phase));
            json_object_object_add(progress, "received", json_object_new_int64(p->received));
            json_object_object_add(progress, "total", json_object_new_int64(p->total));
            json_object_object_add(row, "progress", progress);
            break;
        }
    pthread_mutex_unlock(&s->mutex);
    json_object_object_add(row, "execution_active", json_object_new_boolean(executing));
    return row;
}
json_object *status_runs(Status *s, int64_t id, char *e, size_t n)
{
    return store_runs(s->store, id, e, n);
}
json_object *status_events(Status *s, int64_t id, char *e, size_t n)
{
    return store_events(s->store, id, e, n);
}
json_object *status_library(Status *s, const char *media_type, const char *status, int page,
                            int page_size, char *e, size_t n)
{
    if (!s) {
        fail(e, n, "Status unavailable");
        return NULL;
    }
    return store_library_page(s->store, media_type, status, page, page_size, e, n);
}
json_object *status_steps(Status *s, int64_t id, char *e, size_t n)
{
    return store_steps(s->store, id, e, n);
}
json_object *status_step(Status *s, int64_t run, int64_t step)
{
    return store_step(s->store, run, step);
}

json_object *status_spiders(Status *s, char *e, size_t n)
{
    json_object *rows = store_code_spiders(s->store, e, n);
    for (size_t i = 0; rows && i < json_object_array_length(rows); i++) {
        json_object *row = json_object_array_get_idx(rows, i);
        int64_t id = json_object_get_int64(jg(row, "latest_run_id"));
        if (!id)
            continue;
        json_object *run = status_run(s, id);
        json_object_object_add(row, "execution_active",
                               json_object_new_boolean(jb(run, "execution_active", false)));
        if (jg(run, "progress"))
            json_object_object_add(row, "progress", json_object_get(jg(run, "progress")));
        json_object_put(run);
    }
    return rows;
}
json_object *status_spider(Status *s, int64_t id)
{
    return store_code_spider(s->store, id);
}
json_object *status_history(Status *s, int64_t id, char *e, size_t n)
{
    return store_code_runs(s->store, id, e, n);
}
json_object *status_code_run(Status *s, int64_t id)
{
    if (!store_code_has_run(s->store, id))
        return NULL;
    json_object *run = status_run(s, id);
    if (run)
        json_object_object_del(run, "snapshot");
    return run;
}
