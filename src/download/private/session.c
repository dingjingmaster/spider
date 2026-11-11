#include "session_internal.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
struct HttpSession {
    CURLSH *share;
    pthread_mutex_t mutex;
    DownloadProgress progress;
    void *progress_context;
};
static void cookie_lock(CURL *c, curl_lock_data data, curl_lock_access access, void *ctx)
{
    (void)c;
    (void)data;
    (void)access;
    pthread_mutex_lock(&((HttpSession *)ctx)->mutex);
}
static void cookie_unlock(CURL *c, curl_lock_data data, void *ctx)
{
    (void)c;
    (void)data;
    pthread_mutex_unlock(&((HttpSession *)ctx)->mutex);
}
void download_session_attach(HttpSession *s, CURL *c)
{
    if (s) {
        curl_easy_setopt(c, CURLOPT_SHARE, s->share);
        curl_easy_setopt(c, CURLOPT_COOKIEFILE, "");
    }
}
HttpSession *download_session_create(json_object *cookies, char *e, size_t n)
{
    if (cookies && !json_object_is_type(cookies, json_type_array)) {
        fail(e, n, "Cookies must be an array");
        return NULL;
    }
    HttpSession *s = calloc(1, sizeof(*s));
    if (!s) {
        fail(e, n, "Cannot allocate download session");
        return NULL;
    }
    pthread_mutex_init(&s->mutex, NULL);
    s->share = curl_share_init();
    if (!s->share) {
        download_session_destroy(s);
        fail(e, n, "Cannot create Cookie share");
        return NULL;
    }
    curl_share_setopt(s->share, CURLSHOPT_LOCKFUNC, cookie_lock);
    curl_share_setopt(s->share, CURLSHOPT_UNLOCKFUNC, cookie_unlock);
    curl_share_setopt(s->share, CURLSHOPT_USERDATA, s);
    curl_share_setopt(s->share, CURLSHOPT_SHARE, CURL_LOCK_DATA_COOKIE);
    CURL *c = curl_easy_init();
    if (!c) {
        download_session_destroy(s);
        fail(e, n, "Cannot initialize Cookie transport");
        return NULL;
    }
    download_session_attach(s, c);
    size_t bytes = 0;
    bool ok = true;
    for (size_t i = 0; cookies && i < json_object_array_length(cookies); i++) {
        json_object *cookie = json_object_array_get_idx(cookies, i);
        if (!json_object_is_type(cookie, json_type_string)) {
            ok = false;
            break;
        }
        const char *text = json_object_get_string(cookie);
        bytes += strlen(text);
        if (bytes > 1048576 || curl_easy_setopt(c, CURLOPT_COOKIELIST, text) != CURLE_OK) {
            ok = false;
            break;
        }
    }
    curl_easy_cleanup(c);
    if (!ok) {
        download_session_destroy(s);
        fail(e, n, "Cannot restore Cookie session (limit 1 MiB)");
        return NULL;
    }
    return s;
}
void download_session_destroy(HttpSession *s)
{
    if (s) {
        if (s->share)
            curl_share_cleanup(s->share);
        pthread_mutex_destroy(&s->mutex);
        free(s);
    }
}
json_object *download_session_export(HttpSession *s, char *e, size_t n)
{
    if (!s)
        return json_object_new_array();
    CURL *c = curl_easy_init();
    if (!c) {
        fail(e, n, "Cannot export Cookie session");
        return NULL;
    }
    download_session_attach(s, c);
    struct curl_slist *cookies = NULL;
    CURLcode code = curl_easy_getinfo(c, CURLINFO_COOKIELIST, &cookies);
    json_object *list = json_object_new_array();
    size_t bytes = 0;
    for (struct curl_slist *p = cookies; p; p = p->next) {
        bytes += strlen(p->data);
        if (bytes > 1048576)
            break;
        json_object_array_add(list, json_object_new_string(p->data));
    }
    curl_slist_free_all(cookies);
    curl_easy_cleanup(c);
    if (code != CURLE_OK || bytes > 1048576) {
        json_object_put(list);
        fail(e, n, "Cannot export Cookie session (limit 1 MiB)");
        return NULL;
    }
    return list;
}
HttpSession *download_session_clone(HttpSession *s, char *e, size_t n)
{
    json_object *cookies = download_session_export(s, e, n);
    if (!cookies)
        return NULL;
    HttpSession *copy = download_session_create(cookies, e, n);
    json_object_put(cookies);
    return copy;
}
void download_session_set_progress(HttpSession *s, DownloadProgress callback, void *ctx)
{
    if (s) {
        s->progress = callback;
        s->progress_context = ctx;
    }
}
void download_session_notify(HttpSession *s, int64_t received, int64_t total)
{
    if (s && s->progress)
        s->progress(s->progress_context, received, total);
}
