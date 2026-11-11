#ifndef SPIDER_STATUS_H
#define SPIDER_STATUS_H
#include "storage/storage.h"
typedef struct Status Status;
/* Lifecycle and counters are read from persistent storage. Volatile transport
 * progress is a bounded projection and never overwrites lifecycle state. */
Status *status_create(Store *);
void status_destroy(Status *);
void status_progress(Status *, int64_t run, const char *phase, int64_t received, int64_t total);
void status_forget(Status *, int64_t run);
/* Read-only views restricted to tasks present in the current code catalog. */
json_object *status_spiders(Status *, char *, size_t);
json_object *status_spider(Status *, int64_t);
json_object *status_history(Status *, int64_t spider, char *, size_t);
json_object *status_code_run(Status *, int64_t run);
json_object *status_run(Status *, int64_t);
json_object *status_runs(Status *, int64_t spider, char *, size_t);
json_object *status_events(Status *, int64_t run, char *, size_t);
json_object *status_library(Status *, const char *media_type, const char *status, int page,
                            int page_size, char *, size_t);
json_object *status_steps(Status *, int64_t run, char *, size_t);
json_object *status_step(Status *, int64_t run, int64_t step);
#endif
