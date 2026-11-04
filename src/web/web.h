#ifndef SPIDER_WEB_H
#define SPIDER_WEB_H
#include "service/service.h"
struct mg_context;
struct mg_context *web_start(App *app, char *error, size_t cap);
void web_stop(struct mg_context *context);
#endif
