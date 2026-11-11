#ifndef SPIDER_STORAGE_INTERNAL_H
#define SPIDER_STORAGE_INTERNAL_H
#include "storage/storage.h"
#include <sqlite3.h>
#include <pthread.h>
struct Store {
    sqlite3 *db;
    pthread_mutex_t mutex;
};
int store_open(Store *s, const char *path, char *error, size_t cap);
void store_close(Store *s);
json_object *store_query(Store *s, const char *sql, int64_t id, char *error, size_t cap);
int64_t store_save_spider(Store *s, int64_t id, json_object *rule, char *error, size_t cap);
int store_delete_spider(Store *s, int64_t id, char *error, size_t cap);
int64_t store_start_run(Store *s, int64_t spider_id, char *error, size_t cap);
int store_cancel_run(Store *s, int64_t run);
int64_t store_add_artifact(Store *s, int64_t run, const char *url, const char *path,
                           const char *hash, int64_t bytes);
int wf_initialize(Store *, char *, size_t);
json_object *wf_claim(Store *, char *, size_t);
#endif
