#ifndef SPIDER_ENGINE_H
#define SPIDER_ENGINE_H
#include "service/service.h"
json_object *engine_preview(App *app, json_object *rule, char *error, size_t cap);
void engine_run(App *app, json_object *run);
#endif
