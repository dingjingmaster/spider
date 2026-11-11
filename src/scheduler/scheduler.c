#include "scheduler.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
typedef struct {
    struct Scheduler *scheduler;
    int64_t spider, run;
    pthread_t thread;
} Worker;
struct Scheduler {
    Store *store;
    CrawlerRegistry *registry;
    DownloadClient *downloads;
    Status *status;
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    atomic_bool stopping;
    pthread_t timer;
    bool timer_started, started;
    int count;
    Worker workers[SP_MAX_WORKERS];
};
static void deadline(struct timespec *ts, int ms)
{
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_nsec += (long)ms * 1000000;
    if (ts->tv_nsec >= 1000000000) {
        ts->tv_nsec -= 1000000000;
        ts->tv_sec++;
    }
}
static bool busy(Scheduler *s, int64_t spider)
{
    for (int i = 0; i < s->count; i++)
        if (s->workers[i].spider == spider)
            return true;
    return false;
}
static void *worker_main(void *arg)
{
    Worker *w = arg;
    Scheduler *s = w->scheduler;
    for (;;) {
        pthread_mutex_lock(&s->mutex);
        json_object *run = NULL;
        char error[SP_ERROR] = {0};
        while (!atomic_load(&s->stopping)) {
            int64_t blocked[SP_MAX_WORKERS];
            size_t count = 0;
            for (int i = 0; i < s->count; i++)
                if (s->workers[i].spider)
                    blocked[count++] = s->workers[i].spider;
            run = store_claim_run(s->store, blocked, count, error, sizeof(error));
            if (run) {
                w->run = json_object_get_int64(jg(run, "id"));
                w->spider = json_object_get_int64(jg(run, "spider_id"));
                break;
            }
            if (*error)
                fprintf(stderr, "scheduler: %s\n", error);
            struct timespec until;
            deadline(&until, 100);
            pthread_cond_timedwait(&s->changed, &s->mutex, &until);
        }
        bool stop = atomic_load(&s->stopping);
        pthread_mutex_unlock(&s->mutex);
        if (stop) {
            json_object_put(run);
            break;
        }
        CrawlerContext context = {.spider_id = w->spider,
                                  .run_id = w->run,
                                  .config = jg(run, "snapshot"),
                                  .store = s->store,
                                  .downloads = s->downloads,
                                  .status = s->status,
                                  .stopping = &s->stopping};
        status_progress(s->status, w->run, "starting", 0, 0);
        int rc = crawler_execute(s->registry, &context, error, sizeof(error));
        if (!atomic_load(&s->stopping) && store_finish_run(s->store, w->run,
                                                           rc < 0   ? "failed"
                                                           : rc > 0 ? "partial"
                                                                    : "succeeded",
                                                           rc < 0 ? error : ""))
            fprintf(stderr, "scheduler: cannot persist run result\n");
        store_session_prune(s->store, w->run);
        status_forget(s->status, w->run);
        json_object_put(run);
        /* Retain exclusion until all callback-owned resources have been released.
         * Cancellation may already have changed the persistent lifecycle state. */
        pthread_mutex_lock(&s->mutex);
        w->run = 0;
        w->spider = 0;
        pthread_cond_broadcast(&s->changed);
        pthread_mutex_unlock(&s->mutex);
    }
    return NULL;
}
static void *timer_main(void *arg)
{
    Scheduler *s = arg;
    pthread_mutex_lock(&s->mutex);
    while (!atomic_load(&s->stopping)) {
        char error[SP_ERROR] = {0};
        int64_t now = now_ms();
        json_object *due = store_due(s->store, now, error, sizeof(error));
        if (!due && *error)
            fprintf(stderr, "scheduler: %s\n", error);
        for (size_t i = 0; due && i < json_object_array_length(due); i++) {
            json_object *row = json_object_array_get_idx(due, i);
            int64_t id = json_object_get_int64(jg(row, "id"));
            if (!busy(s, id))
                store_start_run(s->store, id, NULL, 0);
            if (store_next_due(s->store, id, now + (int64_t)ji(row, "interval_seconds", 1) * 1000))
                fprintf(stderr, "scheduler: cannot advance crawler schedule\n");
        }
        json_object_put(due);
        pthread_cond_broadcast(&s->changed);
        struct timespec until;
        deadline(&until, 100);
        pthread_cond_timedwait(&s->changed, &s->mutex, &until);
    }
    pthread_mutex_unlock(&s->mutex);
    return NULL;
}
Scheduler *scheduler_create(Store *store, CrawlerRegistry *registry, DownloadClient *downloads,
                            Status *status)
{
    if (!store || !registry || !downloads || !status)
        return NULL;
    Scheduler *s = calloc(1, sizeof(*s));
    if (s) {
        s->store = store;
        s->registry = registry;
        s->downloads = downloads;
        s->status = status;
        pthread_mutex_init(&s->mutex, NULL);
        pthread_cond_init(&s->changed, NULL);
        atomic_init(&s->stopping, false);
    }
    return s;
}
int scheduler_start(Scheduler *s, int workers, char *e, size_t n)
{
    if (!s || workers < 1 || workers > SP_MAX_WORKERS || s->started || atomic_load(&s->stopping))
        return fail(e, n, "Invalid scheduler start");
    crawler_freeze(s->registry);
    s->started = true;
    /* Publish the complete worker array before any thread can read it. */
    pthread_mutex_lock(&s->mutex);
    s->count = workers;
    int made = 0;
    for (int i = 0; i < workers; i++) {
        s->workers[i].scheduler = s;
        if (pthread_create(&s->workers[i].thread, NULL, worker_main, &s->workers[i]))
            break;
        made++;
    }
    bool ok = made == workers;
    if (ok && pthread_create(&s->timer, NULL, timer_main, s) == 0)
        s->timer_started = true;
    else
        ok = false;
    if (!ok) {
        s->count = made;
        atomic_store(&s->stopping, true);
        pthread_cond_broadcast(&s->changed);
    }
    pthread_mutex_unlock(&s->mutex);
    if (!ok) {
        scheduler_stop(s);
        return fail(e, n, "Cannot start scheduler threads");
    }
    return 0;
}
int64_t scheduler_submit(Scheduler *s, int64_t spider, char *e, size_t n)
{
    if (!s)
        return fail(e, n, "Scheduler unavailable");
    pthread_mutex_lock(&s->mutex);
    int64_t run = -1;
    if (!s->started || atomic_load(&s->stopping))
        fail(e, n, "Scheduler is stopped");
    else if (busy(s, spider))
        fail(e, n, "Crawler is still executing or cancelling");
    else
        run = store_start_run(s->store, spider, e, n);
    if (run > 0)
        pthread_cond_broadcast(&s->changed);
    pthread_mutex_unlock(&s->mutex);
    return run;
}
int scheduler_cancel(Scheduler *s, int64_t run, char *e, size_t n)
{
    pthread_mutex_lock(&s->mutex);
    int rc = store_cancel_run(s->store, run);
    pthread_cond_broadcast(&s->changed);
    pthread_mutex_unlock(&s->mutex);
    return rc ? fail(e, n, "Run is not active") : 0;
}
const atomic_bool *scheduler_stopping(Scheduler *s)
{
    return &s->stopping;
}
void scheduler_stop(Scheduler *s)
{
    if (!s)
        return;
    pthread_mutex_lock(&s->mutex);
    atomic_store(&s->stopping, true);
    pthread_cond_broadcast(&s->changed);
    pthread_mutex_unlock(&s->mutex);
    if (s->timer_started) {
        pthread_join(s->timer, NULL);
        s->timer_started = false;
    }
    for (int i = 0; i < s->count; i++)
        pthread_join(s->workers[i].thread, NULL);
    s->count = 0;
}
void scheduler_destroy(Scheduler *s)
{
    if (!s)
        return;
    scheduler_stop(s);
    pthread_cond_destroy(&s->changed);
    pthread_mutex_destroy(&s->mutex);
    free(s);
}
