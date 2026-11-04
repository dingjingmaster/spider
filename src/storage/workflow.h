#ifndef SPIDER_STORAGE_WORKFLOW_H
#define SPIDER_STORAGE_WORKFLOW_H
#include "storage.h"
/* Returned JSON is owned by the caller. All input JSON is borrowed. An empty
 * claim clears error; a failed claim writes it. No function performs network IO. */
int wf_initialize(Store *, char *, size_t);
json_object *wf_claim(Store *, char *, size_t);
int wf_finish(Store *, json_object *, json_object *, json_object *, json_object *, json_object *,
              bool, char *, size_t);
int wf_fork(Store *, json_object *, json_object *, json_object *, char *, size_t);
json_object *wf_group_results(Store *, int64_t, char *, size_t);
int wf_failure(Store *, json_object *, const char *, const char *, bool, int, char *, size_t);
int wf_cancel(Store *, int64_t, char *, size_t);
#endif
