#ifndef SPIDER_STORAGE_H
#define SPIDER_STORAGE_H
#include "common/common.h"
#include <sqlite3.h>
#include <pthread.h>
typedef struct {
    sqlite3 *db;
    pthread_mutex_t mutex;
} Store;
int store_open(Store *s, const char *path, char *error, size_t cap);
void store_close(Store *s);
json_object *store_query(Store *s, const char *sql, int64_t id, char *error, size_t cap);
int64_t store_save_spider(Store *s, int64_t id, json_object *rule, char *error, size_t cap);
int store_delete_spider(Store *s, int64_t id, char *error, size_t cap);
int64_t store_start_run(Store *s, int64_t spider_id, char *error, size_t cap);
json_object *store_claim_run(Store *s);
int store_finish_run(Store *s, int64_t run, const char *status, const char *error);
int store_cancel_run(Store *s, int64_t run);
bool store_cancelled(Store *s, int64_t run);
int store_event(Store *s, int64_t run, const char *level, const char *message);
int64_t store_enqueue(Store *s, int64_t run, const char *url);
json_object *store_claim_request(Store *s, int64_t run, char *error, size_t cap);
int store_request_status(Store *s, int64_t request, const char *status, const char *error);
int store_commit_page(Store *s, int64_t run, int64_t request, const char *url, json_object *items,
                      const char *next);
int64_t store_add_artifact(Store *s, int64_t run, const char *url, const char *path,
                           const char *hash, int64_t bytes);
void store_schedule(Store *s);
#endif
