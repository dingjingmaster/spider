#ifndef SPIDER_EXTRACT_H
#define SPIDER_EXTRACT_H
#include "common/common.h"
int extract_document(json_object *rule, const char *body, const char *url, json_object **items,
                     char **next, char *error, size_t cap);
#endif
