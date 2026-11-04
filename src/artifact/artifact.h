#ifndef SPIDER_ARTIFACT_H
#define SPIDER_ARTIFACT_H
#include "service/service.h"
#include "model/model.h"
int artifact_download(App *app, int64_t run, json_object *rule, const char *url, cancel_fn cancel,
                      void *context, char *error, size_t cap);
#endif
