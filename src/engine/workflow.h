#ifndef SPIDER_ENGINE_WORKFLOW_H
#define SPIDER_ENGINE_WORKFLOW_H
#include "service/service.h"
void workflow_execute(App *app, json_object *job);
json_object *workflow_preview(App *app, json_object *payload, char *error, size_t cap);
#endif
