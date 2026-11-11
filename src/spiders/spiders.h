#ifndef SPIDER_SPIdERS_H
#define SPIDER_SPIdERS_H
#include "crawler/crawler.h"
/* The composition root calls this once, before starting the scheduler.
 * Add each compiled site definition here; scheduler code never changes. */
int spiders_register(CrawlerRegistry *, char *, size_t);
#endif
