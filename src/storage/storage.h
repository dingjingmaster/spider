#ifndef SPIDER_STORAGE_H
#define SPIDER_STORAGE_H
#include "common/common.h"
#include <stdbool.h>
typedef struct Store Store;
/* Returned JSON/Store objects are owned by the caller; input JSON is borrowed.
 * All operations synchronize internally. No network or site logic lives here. */
Store *store_create(const char *path, char *error, size_t cap);
void store_destroy(Store *store);
/* Code task catalog: sync is atomic and preserves stable IDs/history. */
int store_sync_code(Store *, json_object *tasks, char *, size_t);
json_object *store_code_spiders(Store *, char *, size_t);
json_object *store_code_spider(Store *, int64_t);
json_object *store_code_runs(Store *, int64_t spider, char *, size_t);
bool store_code_has_run(Store *, int64_t run);
int64_t store_save_spider(Store *, int64_t, json_object *, char *, size_t);
int store_delete_spider(Store *, int64_t, char *, size_t);
int64_t store_start_run(Store *, int64_t, char *, size_t);
int store_cancel_run(Store *, int64_t);
int64_t store_add_artifact(Store *, int64_t, const char *, const char *, const char *, int64_t);
json_object *store_spiders(Store *, char *, size_t);
json_object *store_spider(Store *, int64_t);
json_object *store_runs(Store *, int64_t, char *, size_t);
json_object *store_run(Store *, int64_t);
json_object *store_items(Store *, int64_t, int64_t, char *, size_t);
json_object *store_events(Store *, int64_t, char *, size_t);
int store_event(Store *, int64_t run_id, const char *level, const char *message, char *error,
                size_t cap);
json_object *store_steps(Store *, int64_t, char *, size_t);
json_object *store_step(Store *, int64_t, int64_t);
json_object *store_artifacts(Store *, int64_t, char *, size_t);
json_object *store_artifact(Store *, int64_t);
json_object *store_due(Store *, int64_t, char *, size_t);
int store_next_due(Store *, int64_t, int64_t);
json_object *store_claim_run(Store *, const int64_t *, size_t, char *, size_t);
int store_finish_run(Store *, int64_t, const char *, const char *);
bool store_run_active(Store *, int64_t);
int store_record_request(Store *, json_object *, json_object *, const char *, char *, size_t);
bool store_step_has_post(Store *, int64_t);
int64_t store_step_iteration(Store *, int64_t);
json_object *store_session_load(Store *, int64_t, char *, size_t);
int store_session_save(Store *, int64_t, json_object *, char *, size_t);
void store_session_prune(Store *, int64_t);
int store_emit(Store *, int64_t, const char *, const char *, json_object *, char *, size_t);
int store_export(Store *, int64_t, bool, char **, size_t *, char *, size_t);
/* Generic download catalog shared by all media types. */
int store_download_scan(Store *, int64_t run_id, json_object *source, json_object *items,
                        char *error, size_t cap);
json_object *store_download_queue(Store *, const char *spider_code, const char *media_type,
                                  int limit, char *error, size_t cap);
int store_download_start(Store *, const char *download_url, int64_t run_id, char *error,
                         size_t cap);
int store_download_finish(Store *, const char *download_url, const char *status,
                          const char *target_path, const char *local_sha256,
                          int64_t local_bytes, const char *error_text, char *error, size_t cap);
int store_download_decide(Store *, const char *download_url, const char *decision, int curated,
                          char *error, size_t cap);
json_object *store_download_summary(Store *, const char *spider_code, char *error, size_t cap);
int wf_initialize_run(Store *, int64_t, char *, size_t);
json_object *wf_claim_run(Store *, int64_t, char *, size_t);
/* Returned JSON is owned by the caller. All input JSON is borrowed. An empty
 * claim clears error; a failed claim writes it. No function performs network IO. */
int wf_finish(Store *, json_object *, json_object *, json_object *, json_object *, json_object *,
              bool, char *, size_t);
int wf_fork(Store *, json_object *, json_object *, json_object *, char *, size_t);
json_object *wf_group_results(Store *, int64_t, char *, size_t);
int wf_failure(Store *, json_object *, const char *, const char *, bool, int, char *, size_t);
int wf_cancel(Store *, int64_t, char *, size_t);
#endif
