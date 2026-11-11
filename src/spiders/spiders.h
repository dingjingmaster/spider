#ifndef SPIDER_SPIDERS_H
#define SPIDER_SPIDERS_H
#include "crawler/crawler.h"
/* The composition root calls this once, before starting the scheduler.
 * Each site registers its implementation and crawler_add() task configuration.
 * Scheduler code never changes when a site is added. */
int spiders_register(CrawlerRegistry *, char *, size_t);
#endif
