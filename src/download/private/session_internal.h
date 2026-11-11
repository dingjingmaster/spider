#ifndef SPIDER_DOWNLOAD_SESSION_INTERNAL_H
#define SPIDER_DOWNLOAD_SESSION_INTERNAL_H
#include "download/download.h"
#include <curl/curl.h>
void download_session_attach(HttpSession *, CURL *);
void download_session_notify(HttpSession *, int64_t, int64_t);
#endif
