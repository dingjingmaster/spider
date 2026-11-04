#ifndef SPIDER_RULE_DATA_H
#define SPIDER_RULE_DATA_H
#include "common/common.h"
/* Inputs are borrowed. Only successful input_prepare replaces context.input;
 * transform returns an owned, isolated output and never modifies context. */
int flow_schema_validate(json_object *schema, char *error, size_t cap);
int flow_input_prepare(json_object *step, json_object *context, char *error, size_t cap);
int flow_transform_validate(json_object *step, char *error, size_t cap);
int flow_transform(json_object *step, json_object *context, json_object **output, char *error,
                   size_t cap);
#endif
