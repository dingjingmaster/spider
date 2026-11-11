#ifndef SPIDER_WEB_H
#define SPIDER_WEB_H
#include "scheduler/scheduler.h"
typedef struct Web Web;
typedef struct {
    const char *listen, *ui_directory, *token;
} WebConfig;
/* Copies configuration. Stop Web before destroying its borrowed modules. */
Web *web_start(const WebConfig *, Store *, Scheduler *, CrawlerRegistry *, Status *, char *,
               size_t);
void web_stop(Web *);
#endif
