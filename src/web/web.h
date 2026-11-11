#ifndef SPIDER_WEB_H
#define SPIDER_WEB_H
#include "status/status.h"
typedef struct Web Web;
/* Submits one run for a current code crawler. Returns a run id or -1. */
typedef int64_t (*WebSubmit)(void *context, int64_t spider_id, char *error, size_t cap);
typedef struct {
    const char *listen, *ui_directory, *token;
    WebSubmit submit;
    void *submit_context;
} WebConfig;
/* Copies config and borrows Status until web_stop. submit may be NULL. */
Web *web_start(const WebConfig *, Status *, char *, size_t);
void web_stop(Web *);
#endif
