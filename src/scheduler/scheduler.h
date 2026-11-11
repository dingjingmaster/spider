#ifndef SPIDER_SCHEDULER_H
#define SPIDER_SCHEDULER_H
#include "crawler/crawler.h"
#define SP_MAX_WORKERS 16
typedef struct Scheduler Scheduler;
/* All dependencies must outlive the scheduler. Registry freezes at start. */
Scheduler *scheduler_create(Store *, CrawlerRegistry *, DownloadClient *, Status *);
int scheduler_start(Scheduler *, int workers, char *, size_t);
int64_t scheduler_submit(Scheduler *, int64_t spider, char *, size_t);
int scheduler_cancel(Scheduler *, int64_t run, char *, size_t);
const atomic_bool *scheduler_stopping(Scheduler *);
void scheduler_stop(Scheduler *);
void scheduler_destroy(Scheduler *);
#endif
