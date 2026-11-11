#ifndef SPIDER_WEB_H
#define SPIDER_WEB_H
#include "status/status.h"
typedef struct Web Web;
typedef struct {
    const char *listen, *ui_directory, *token;
} WebConfig;
/* Read-only server. Copies config and borrows Status until web_stop. */
Web *web_start(const WebConfig *, Status *, char *, size_t);
void web_stop(Web *);
#endif
