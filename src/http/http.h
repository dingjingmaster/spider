#ifndef SPIDER_HTTP_H
#define SPIDER_HTTP_H
#include "model/model.h"
#include "common/common.h"
#include <stdio.h>
#include "session.h"
int http_fetch(json_object *rule, const char *url, FILE *file, cancel_fn cancel, void *context,
               HttpResponse *response, char *error, size_t cap);
void http_response_free(HttpResponse *r);
int http_fetch_session(json_object *rule, const char *url, FILE *file, cancel_fn cancel,
                       void *context, HttpSession *session, HttpResponse *response, char *error,
                       size_t cap);
#endif
