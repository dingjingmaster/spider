#ifndef SPIDER_COMMON_REDACT_H
#define SPIDER_COMMON_REDACT_H
#include "common.h"
/* Presentation-only owned copy. Never modifies execution values or rule. */
json_object *debug_redact(json_object *value, json_object *rule);
json_object *debug_redact_with_context(json_object *value, json_object *rule, json_object *source);
/* Branch-local, bounded sensitive-value provenance. Atomic on failure. */
int debug_track_secrets(json_object *context, json_object *output, json_object *rule, char *error,
                        size_t cap);
int debug_track_transform(json_object *context, json_object *step, json_object *output,
                          json_object *rule, char *error, size_t cap);
#endif
