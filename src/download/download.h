#ifndef SPIDER_DOWNLOAD_H
#define SPIDER_DOWNLOAD_H
#include "common/common.h"
#include "model/model.h"
#include <stdio.h>
typedef struct DownloadClient DownloadClient;
typedef struct HttpSession HttpSession;
typedef void (*DownloadProgress)(void *, int64_t received, int64_t total);
typedef int64_t (*DownloadRecord)(void *, const char *url, const char *path, const char *sha256,
                                  int64_t bytes);
typedef struct {
    int64_t group_id;
    json_object *options;
    const char *url;
    HttpSession *session;
    cancel_fn cancelled;
    void *cancel_context;
    DownloadRecord record;
    void *record_context;
} DownloadRequest;
typedef struct {
    int64_t id, bytes;
    long http_status;
    bool skipped;
    char path[SP_PATH], sha256[65];
} DownloadFile;
/* Client owns its directory and publication lock. It has no database dependency.
 * record runs after publication under the lock; failure rolls back only owned files. */
DownloadClient *download_create(const char *directory, char *, size_t);
void download_destroy(DownloadClient *);
int download_save(DownloadClient *, const DownloadRequest *, DownloadFile *, char *, size_t);
/* A session is owned by one run; cookie transport is independent of persistence. */
HttpSession *download_session_create(json_object *cookies, char *, size_t);
HttpSession *download_session_clone(HttpSession *, char *, size_t);
void download_session_destroy(HttpSession *);
json_object *download_session_export(HttpSession *, char *, size_t);
void download_session_set_progress(HttpSession *, DownloadProgress, void *);
/* Default credential/body origin is options.url when provided, otherwise the
 * separately supplied URL. Explicit origin restrictions are never widened. */
int download_fetch(json_object *, const char *, FILE *, cancel_fn, void *, HttpSession *,
                   HttpResponse *, char *, size_t);
void download_response_free(HttpResponse *);
#endif
