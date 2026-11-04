#ifndef SPIDER_SERVICE_H
#define SPIDER_SERVICE_H
#include "storage/storage.h"
#include <stdatomic.h>
#include "http/session.h"
#define SP_MAX_WORKERS 16
typedef struct {
    Store store;
    atomic_bool stopping;
    pthread_t workers[SP_MAX_WORKERS];
    int worker_count;
    char data_dir[SP_PATH];
    char ui_dir[SP_PATH];
    char listen[128];
    char token[256];
    SessionPool *sessions;
    struct PreviewPool *previews;
    pthread_mutex_t artifact_mutex;
} App;
int service_start(App *app, int workers, char *error, size_t cap);
void service_stop(App *app);
#endif
