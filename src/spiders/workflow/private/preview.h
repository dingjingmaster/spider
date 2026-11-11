#ifndef SPIDER_ENGINE_PREVIEW_H
#define SPIDER_ENGINE_PREVIEW_H
#include "crawler/crawler.h"
#include <pthread.h>
typedef struct PreviewPool PreviewPool;
PreviewPool *preview_pool_create(void);
/* Call only after web handlers have stopped. */
void preview_pool_destroy(PreviewPool *pool);
/* Busy sessions are retired immediately and freed when their last handler exits. */
int preview_pool_release(PreviewPool *pool, const char *id, char *error, size_t cap);
json_object *workflow_preview_step(PreviewPool *, const atomic_bool *, json_object *, char *,
                                   size_t);
#endif
