#ifndef SPIDER_RULE_H
#define SPIDER_RULE_H
#include "common/common.h"
int rule_validate(json_object *rule, char *error, size_t cap);
/* Internal rendered HTTP/extraction configuration, not a public rule schema. */
int request_validate(json_object *request, char *error, size_t cap);
int selector_valid(const char *text);
#endif
