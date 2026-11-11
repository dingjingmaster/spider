/* Real native callbacks pin whole-run exclusion, including cancellation teardown.
 * Removing scheduler's active-spider guard makes the cancellation assertions fail. */
#include "scheduler/scheduler.h"
#include <assert.h>
#include <curl/curl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static int entered[2], active[2], maximum[2], snapshots[2];
static bool released[2], cancel_seen;
static int native_run(CrawlerContext *c, char *e, size_t n)
{
    int slot = ji(c->config, "slot", 0);
    pthread_mutex_lock(&mutex);
    int round = ++entered[slot];
    active[slot]++;
    if (active[slot] > maximum[slot])
        maximum[slot] = active[slot];
    snapshots[slot] = ji(c->config, "marker", 0);
    pthread_mutex_unlock(&mutex);
    status_progress(c->status, c->run_id, "native callback", 42, 100);
    if (round == 1) {
        for (;;) {
            bool cancelled = crawler_cancelled(c);
            pthread_mutex_lock(&mutex);
            if (cancelled && slot == 0)
                cancel_seen = true;
            bool ready = released[slot];
            pthread_mutex_unlock(&mutex);
            if (ready || atomic_load(c->stopping))
                break;
            sleep_ms(10);
        }
    }
    int rc = 0;
    if (!crawler_cancelled(c)) {
        json_object *item = json_tokener_parse("{\"saved\":true}");
        rc = store_emit(c->store, c->run_id, "one", "native://fixture", item, e, n);
        json_object_put(item);
    }
    pthread_mutex_lock(&mutex);
    active[slot]--;
    pthread_mutex_unlock(&mutex);
    return rc;
}
static bool reached(int first, int second, bool cancelled)
{
    pthread_mutex_lock(&mutex);
    bool ok = entered[0] >= first && entered[1] >= second && (!cancelled || cancel_seen);
    pthread_mutex_unlock(&mutex);
    return ok;
}
static void wait_reached(int first, int second, bool cancelled)
{
    for (int i = 0; i < 300 && !reached(first, second, cancelled); i++)
        sleep_ms(10);
    assert(reached(first, second, cancelled));
}
static void wait_finished(Store *store, int64_t run)
{
    for (int i = 0; i < 300; i++) {
        json_object *row = store_run(store, run);
        assert(row);
        bool done = !strcmp(js(row, "status", ""), "succeeded");
        json_object_put(row);
        if (done)
            return;
        sleep_ms(10);
    }
    assert(!"native crawler stalled");
}
int main(void)
{
    assert(curl_global_init(CURL_GLOBAL_DEFAULT) == 0);
    char e[SP_ERROR] = {0}, directory[] = "/tmp/spider-native-XXXXXX";
    assert(mkdtemp(directory));
    Store *store = store_create(":memory:", e, sizeof(e));
    assert(store);
    DownloadClient *downloads = download_create(directory, e, sizeof(e));
    assert(downloads);
    Status *status = status_create(store);
    CrawlerRegistry *registry = crawler_create();
    assert(status && registry);
    CrawlerDefinition a = {.id = "native-a", .name = "Native A", .run = native_run};
    CrawlerDefinition b = {.id = "native-b", .name = "Native B", .run = native_run};
    assert(!crawler_register(registry, &a, e, sizeof(e)));
    assert(!crawler_register(registry, &b, e, sizeof(e)));
    assert(crawler_register(registry, &a, e, sizeof(e)) < 0);
    json_object *config = json_tokener_parse("{\"name\":\"A\",\"implementation\":\"missing\","
                                             "\"slot\":0,\"marker\":7,\"interval_seconds\":1}");
    assert(crawler_validate(registry, config, e, sizeof(e)) < 0);
    json_object_object_add(config, "implementation", json_object_new_string("native-a"));
    assert(!crawler_validate(registry, config, e, sizeof(e)));
    int64_t sa = store_save_spider(store, 0, config, e, sizeof(e));
    assert(sa > 0);
    json_object *other = json_tokener_parse(
        "{\"name\":\"B\",\"implementation\":\"native-b\",\"slot\":1,\"marker\":8}");
    int64_t sb = store_save_spider(store, 0, other, e, sizeof(e));
    assert(sb > 0);
    Scheduler *scheduler = scheduler_create(store, registry, downloads, status);
    assert(scheduler);
    assert(!scheduler_start(scheduler, 2, e, sizeof(e)));
    assert(crawler_register(registry, &a, e, sizeof(e)) < 0);
    int64_t ra = scheduler_submit(scheduler, sa, e, sizeof(e));
    assert(ra > 0);
    int64_t rb = scheduler_submit(scheduler, sb, e, sizeof(e));
    assert(rb > 0);
    wait_reached(1, 1, false);
    assert(scheduler_submit(scheduler, sa, e, sizeof(e)) < 0);
    assert(scheduler_submit(scheduler, sb, e, sizeof(e)) < 0);
    json_object *view = status_run(status, ra);
    assert(view && ji(jg(view, "progress"), "received", 0) == 42);
    json_object_put(view);
    sleep_ms(1200); /* The timer became due while both real callbacks remained blocked. */
    pthread_mutex_lock(&mutex);
    assert(entered[0] == 1 && entered[1] == 1);
    pthread_mutex_unlock(&mutex);
    json_object_object_add(config, "enabled", json_object_new_boolean(false));
    json_object_object_add(config, "marker", json_object_new_int(99));
    assert(store_save_spider(store, sa, config, e, sizeof(e)) == sa);
    assert(!scheduler_cancel(scheduler, ra, e, sizeof(e)));
    wait_reached(1, 1, true);
    for (int i = 0; i < 10; i++)
        assert(scheduler_submit(scheduler, sa, e, sizeof(e)) < 0);
    pthread_mutex_lock(&mutex);
    assert(snapshots[0] == 7);
    released[0] = released[1] = true;
    pthread_mutex_unlock(&mutex);
    int64_t again = -1;
    for (int i = 0; i < 300 && again < 0; i++) {
        again = scheduler_submit(scheduler, sa, e, sizeof(e));
        if (again < 0)
            sleep_ms(10);
    }
    assert(again > ra);
    wait_finished(store, again);
    wait_finished(store, rb);
    json_object *items = store_items(store, again, 0, e, sizeof(e));
    assert(items && json_object_array_length(items) == 1);
    assert(jb(jg(json_object_array_get_idx(items, 0), "data"), "saved", false));
    json_object_put(items);
    pthread_mutex_lock(&mutex);
    assert(maximum[0] == 1 && maximum[1] == 1 && snapshots[0] == 99);
    pthread_mutex_unlock(&mutex);
    scheduler_stop(scheduler);
    assert(scheduler_submit(scheduler, sa, e, sizeof(e)) < 0);
    scheduler_destroy(scheduler);
    crawler_destroy(registry);
    status_destroy(status);
    download_destroy(downloads);
    store_destroy(store);
    json_object_put(config);
    json_object_put(other);
    rmdir(directory);
    curl_global_cleanup();
    puts("native scheduler tests passed");
    return 0;
}
