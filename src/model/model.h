#ifndef SPIDER_MODEL_H
#define SPIDER_MODEL_H
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
/* Persisted state names are part of the API contract. */
#define RUN_QUEUED "queued"
#define RUN_RUNNING "running"
#define RUN_SUCCEEDED "succeeded"
#define RUN_FAILED "failed"
#define RUN_CANCELLED "cancelled"
typedef int (*cancel_fn)(void *context);
typedef struct {
    char *body;
    size_t size;
    long status;
    char *url;
    char *content_type;
} HttpResponse;
#endif
