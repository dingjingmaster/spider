#ifndef SPIDER_ENGINE_WORKFLOW_H
#define SPIDER_ENGINE_WORKFLOW_H
#include "service/service.h"
#include "model/model.h"
void workflow_execute(App *app, json_object *job);
json_object *workflow_preview(App *app, json_object *payload, char *error, size_t cap);
/* Shared deterministic phases used by durable runs and temporary debug sessions. */
json_object *workflow_context(json_object *rule, json_object *supplied);
json_object *workflow_request(json_object *rule, json_object *step, json_object *context,
                              char *error, size_t cap);
json_object *workflow_response(json_object *step, json_object *request, HttpResponse *response,
                               char *error, size_t cap);
int workflow_complete_context(json_object *rule, json_object *step, json_object *context,
                              json_object *output, char *error, size_t cap);
#endif
