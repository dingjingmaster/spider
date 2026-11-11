#ifndef SPIDER_RULE_H
#define SPIDER_RULE_H
#include "common/common.h"
int rule_validate(json_object *rule, char *error, size_t cap);
/* Internal rendered HTTP/extraction configuration, not a public rule schema. */
int request_validate(json_object *request, char *error, size_t cap);
int selector_valid(const char *text);

json_object *flow_step(json_object *rule, const char *id);
json_object *flow_clone(json_object *value);
int flow_validate(json_object *rule, char *error, size_t cap);
int flow_render(json_object *value, json_object *context, json_object **output, char *error,
                size_t cap);
int flow_test(json_object *test, json_object *context, bool *result, char *error, size_t cap);
/* Inputs are borrowed. Only successful input_prepare replaces context.input;
 * transform returns an owned, isolated output and never modifies context. */
int flow_schema_validate(json_object *schema, char *error, size_t cap);
int flow_input_prepare(json_object *step, json_object *context, char *error, size_t cap);
int flow_transform_validate(json_object *step, char *error, size_t cap);
int flow_transform(json_object *step, json_object *context, json_object **output, char *error,
                   size_t cap);
/* Database upgrade only. New API writes must use workflow schema v2. */
json_object *rule_upgrade_v1(json_object *old, char *error, size_t cap);
#endif
