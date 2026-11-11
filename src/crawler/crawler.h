#ifndef SPIDER_CRAWLER_H
#define SPIDER_CRAWLER_H
#include "common/common.h"
#include "download/download.h"
#include "storage/storage.h"
#include "status/status.h"
#include <stdatomic.h>
typedef struct CrawlerRegistry CrawlerRegistry;
/* Borrowed module handles and immutable run snapshot, owned by the scheduler.
 * A run callback and all its requests execute on one worker thread.
 * The callback must not spawn threads or retain this context after returning. */
typedef struct {
    int64_t spider_id, run_id;
    json_object *config;
    Store *store;
    DownloadClient *downloads;
    Status *status;
    HttpSession *session;
    const atomic_bool *stopping;
} CrawlerContext;
typedef struct {
    const char *id;
    const char *name;
    int (*validate)(json_object *, char *, size_t);
    /* Return 0 success, 1 partial, -1 failure. Respect crawler_cancelled(). */
    int (*run)(CrawlerContext *, char *, size_t);
    json_object *(*preview)(void *, json_object *, bool single_step, const atomic_bool *, char *,
                            size_t);
    int (*release_preview)(void *, const char *, char *, size_t);
    void (*destroy)(void *);
    void *userdata;
} CrawlerDefinition;
CrawlerRegistry *crawler_create(void);
void crawler_destroy(CrawlerRegistry *);
/* Definitions are copied; userdata ownership transfers on successful register.
 * Register before scheduler_start. Frozen registries reject subsequent writes. */
int crawler_register(CrawlerRegistry *, const CrawlerDefinition *, char *, size_t);
/* Code-owned runnable tasks. config is cloned; key is stable across releases.
 * Register tasks before scheduler_start, then synchronize once during startup. */
int crawler_add(CrawlerRegistry *, const char *key, const char *implementation, json_object *config,
                char *, size_t);
int crawler_sync(CrawlerRegistry *, Store *, char *, size_t);
void crawler_freeze(CrawlerRegistry *);
json_object *crawler_definitions(CrawlerRegistry *);
int crawler_validate(CrawlerRegistry *, json_object *, char *, size_t);
int crawler_execute(CrawlerRegistry *, CrawlerContext *, char *, size_t);
json_object *crawler_preview(CrawlerRegistry *, json_object *, bool, const atomic_bool *, char *,
                             size_t);
int crawler_release_preview(CrawlerRegistry *, const char *, const char *, char *, size_t);
int crawler_cancelled(void *context);
int crawler_fetch(CrawlerContext *, json_object *, const char *, HttpResponse *, char *, size_t);
int crawler_save_file(CrawlerContext *, json_object *, const char *, DownloadFile *, char *,
                      size_t);
#endif
