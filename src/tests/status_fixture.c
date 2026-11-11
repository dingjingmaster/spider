/* A test application declares actual C crawlers, then uses the production
 * scheduler and read-only Web. No fixture routes are added to the product. */
#include "crawler/crawler.h"
#include "scheduler/scheduler.h"
#include "web/web.h"
#include <curl/curl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static volatile sig_atomic_t stopping;
static void signal_stop(int sig)
{
    (void)sig;
    stopping = 1;
}
static int work(CrawlerContext *context, char *e, size_t n)
{
    status_progress(context->status, context->run_id, "collecting", 10, 100);
    for (int i = 0; i < 10; i++) {
        if (crawler_cancelled(context))
            return -1;
        sleep_ms(20);
    }
    return jb(context->config, "fail", false) ? fail(e, n, "fixture download failed") : 0;
}
static int64_t submit_run(void *context, int64_t spider, char *error, size_t cap)
{
    return scheduler_submit(context, spider, error, cap);
}
static int add(CrawlerRegistry *registry, const char *key, const char *raw, char *e, size_t n)
{
    json_object *config = json_tokener_parse(raw);
    int rc = crawler_add(registry, key, "fixture", config, e, n);
    json_object_put(config);
    return rc;
}
int main(int argc, char **argv)
{
    const char *directory = NULL, *ui = NULL, *port = NULL;
    bool remove_failed = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--remove-failed")) {
            remove_failed = true;
            continue;
        }
        if (i + 1 >= argc)
            return 2;
        if (!strcmp(argv[i], "--data-dir"))
            directory = argv[++i];
        else if (!strcmp(argv[i], "--ui-dir"))
            ui = argv[++i];
        else if (!strcmp(argv[i], "--port"))
            port = argv[++i];
        else
            return 2;
    }
    if (!directory || !ui || !port || mkdirs(directory) || curl_global_init(CURL_GLOBAL_DEFAULT))
        return 2;
    char path[SP_PATH], e[SP_ERROR] = {0}, listen[128];
    snprintf(path, sizeof(path), "%s/spider.db", directory);
    Store *store = store_create(path, e, sizeof(e));
    if (!store)
        return 2;
    /* Persist one legacy task. Production startup sync must prevent its execution. */
    json_object *legacy = json_tokener_parse(
        "{\"name\":\"legacy web task\",\"interval_seconds\":1,\"enabled\":true}");
    int64_t old = store_save_spider(store, 0, legacy, e, sizeof(e));
    if (old < 1)
        return 2;
    if (store_start_run(store, old, e, sizeof(e)) < 1)
        return 2;
    json_object_put(legacy);
    CrawlerRegistry *registry = crawler_create();
    CrawlerDefinition definition = {.id = "fixture", .name = "Fixture", .run = work};
    if (!registry || crawler_register(registry, &definition, e, sizeof(e)))
        return 2;
    if (add(registry, "good",
            "{\"name\":\"Success "
            "crawler\",\"interval_seconds\":1,\"headers\":{\"Authorization\":\"Bearer "
            "fixture-secret\"}}",
            e, sizeof(e)) ||
        (!remove_failed &&
         add(registry, "bad",
             "{\"name\":\"Failure "
             "crawler\",\"interval_seconds\":1,\"fail\":true,\"password\":\"fixture-secret\"}",
             e, sizeof(e))) ||
        add(registry, "idle",
            "{\"name\":\"Idle crawler\",\"interval_seconds\":0,\"enabled\":false}", e, sizeof(e)) ||
        crawler_sync(registry, store, e, sizeof(e)))
        return 2;
    snprintf(path, sizeof(path), "%s/downloads", directory);
    DownloadClient *downloads = download_create(path, e, sizeof(e));
    Status *status = status_create(store);
    Scheduler *scheduler = scheduler_create(store, registry, downloads, status);
    if (!scheduler || scheduler_start(scheduler, 2, e, sizeof(e)))
        return 2;
    snprintf(listen, sizeof(listen), "127.0.0.1:%s", port);
    WebConfig config = {.listen = listen, .ui_directory = ui, .token = "fixture-token",
                        .submit = submit_run, .submit_context = scheduler};
    Web *web = web_start(&config, status, e, sizeof(e));
    if (!web)
        return 2;
    signal(SIGTERM, signal_stop);
    signal(SIGINT, signal_stop);
    while (!stopping)
        sleep_ms(20);
    scheduler_stop(scheduler);
    web_stop(web);
    scheduler_destroy(scheduler);
    status_destroy(status);
    download_destroy(downloads);
    crawler_destroy(registry);
    store_destroy(store);
    curl_global_cleanup();
    return 0;
}
