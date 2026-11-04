#ifndef SPIDER_WORKFLOW_RULE_H
#define SPIDER_WORKFLOW_RULE_H
#include "common/common.h"
json_object *flow_step(json_object *rule, const char *id);
json_object *flow_clone(json_object *value);
int flow_validate(json_object *rule, char *error, size_t cap);
int flow_render(json_object *value, json_object *context, json_object **output, char *error,
                size_t cap);
int flow_test(json_object *test, json_object *context, bool *result, char *error, size_t cap);
#endif
