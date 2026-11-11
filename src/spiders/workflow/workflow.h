#ifndef SPIDER_WORKFLOW_IMPLEMENTATION_H
#define SPIDER_WORKFLOW_IMPLEMENTATION_H
#include "crawler/crawler.h"
/* JSON workflows implement the same serial run contract as native C spiders. */
int workflow_register(CrawlerRegistry *, char *, size_t);
#endif
