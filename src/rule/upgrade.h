#ifndef SPIDER_RULE_UPGRADE_H
#define SPIDER_RULE_UPGRADE_H
#include "common/common.h"
/* Database upgrade only. New API writes must use workflow schema v2. */
json_object *rule_upgrade_v1(json_object *old, char *error, size_t cap);
#endif
