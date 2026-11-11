/* Tests-only engine control fixture. Never linked into the product executable. */
#ifndef SPIDER_TEST_ENGINE_WEB_H
#define SPIDER_TEST_ENGINE_WEB_H
#include "scheduler/scheduler.h"
typedef struct EngineWeb EngineWeb;
typedef struct {
    const char *listen, *ui_directory, *token;
} EngineWebConfig;
/* Copies configuration. Stop EngineWeb before destroying its borrowed modules. */
EngineWeb *engine_web_start(const EngineWebConfig *, Store *, Scheduler *, CrawlerRegistry *,
                            Status *, char *, size_t);
void engine_web_stop(EngineWeb *);
#endif
