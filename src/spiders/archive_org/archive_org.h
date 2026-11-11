#ifndef SPIDER_ARCHIVE_ORG_H
#define SPIDER_ARCHIVE_ORG_H
#include "crawler/crawler.h"

int archive_org_register(CrawlerRegistry *registry, char *error, size_t cap);
int archive_org_validate(json_object *config, char *error, size_t cap);
int archive_org_run(CrawlerContext *context, char *error, size_t cap);
#endif
