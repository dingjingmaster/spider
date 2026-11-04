#ifndef SPIDER_HTTP_SESSION_H
#define SPIDER_HTTP_SESSION_H
#include "storage/storage.h"
#include <curl/curl.h>
typedef struct HttpSession HttpSession;
typedef struct SessionPool SessionPool;
SessionPool *session_pool_create(Store *store);
void session_pool_destroy(SessionPool *pool);
HttpSession *session_acquire(SessionPool *pool, int64_t run, char *error, size_t cap);
void session_release(SessionPool *pool, HttpSession *session);
void session_pool_prune(SessionPool *pool, int64_t run);
HttpSession *session_temporary(void);
void session_destroy(HttpSession *session);
void session_attach(HttpSession *session, CURL *curl);
int session_persist(HttpSession *session, char *error, size_t cap);
#endif
