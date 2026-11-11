#include "download/download.h"
#include "session_internal.h"
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
typedef struct {
    HttpResponse *r;
    FILE *file;
    size_t max;
    cancel_fn cancel;
    void *context;
    HttpSession *session;
    bool too_large;
    bool io_error;
} Transfer;
static size_t receive(char *data, size_t size, size_t count, void *context)
{
    Transfer *t = context;
    if (size && count > SIZE_MAX / size)
        return 0;
    size_t bytes = size * count;
    if (bytes > t->max - t->r->size) {
        t->too_large = true;
        return 0;
    }
    if (t->file) {
        if (fwrite(data, 1, bytes, t->file) != bytes) {
            t->io_error = true;
            return 0;
        }
    } else {
        char *p = realloc(t->r->body, t->r->size + bytes + 1);
        if (!p)
            return 0;
        t->r->body = p;
        memcpy(p + t->r->size, data, bytes);
        p[t->r->size + bytes] = 0;
    }
    t->r->size += bytes;
    return bytes;
}
static int progress(void *context, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d)
{

    (void)c;
    (void)d;
    Transfer *t = context;
    download_session_notify(t->session, (int64_t)b, (int64_t)a);
    return t->cancel ? t->cancel(t->context) : 0;
}
void download_response_free(HttpResponse *r)
{
    free(r->body);
    free(r->url);
    free(r->content_type);
    memset(r, 0, sizeof(*r));
}
static int perform_once(json_object *rule, const char *url, FILE *file, cancel_fn cancel,
                        void *context, HttpResponse *r, char **redirect, bool post,
                        bool send_headers, long timeout_ms, HttpSession *session, char *e, size_t n)
{
    memset(r, 0, sizeof(*r));
    if (!url_valid(url))
        return fail(e, n, "Invalid HTTP URL");
    CURL *c = curl_easy_init();
    if (!c)
        return fail(e, n, "Cannot initialize HTTP transfer");
    download_session_attach(session, c);
    Transfer t = {.r = r,
                  .file = file,
                  .max = (size_t)ji(rule, "max_bytes", 4194304),
                  .cancel = cancel,
                  .context = context,
                  .session = session};
    char curl_error[CURL_ERROR_SIZE] = {0};
    struct curl_slist *headers = NULL;
    json_object *h = jg(rule, "headers");
    if (h && send_headers) {
        json_object_object_foreach(h, k, v)
        {
            size_t len = strlen(k) + strlen(json_object_get_string(v)) + 3;
            char *line = malloc(len);
            if (!line) {
                curl_slist_free_all(headers);
                curl_easy_cleanup(c);
                return fail(e, n, "Out of memory");
            }
            snprintf(line, len, "%s: %s", k, json_object_get_string(v));
            struct curl_slist *added = curl_slist_append(headers, line);
            free(line);
            if (!added) {
                curl_slist_free_all(headers);
                curl_easy_cleanup(c);
                return fail(e, n, "Out of memory");
            }
            headers = added;
        }
    }
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(c, CURLOPT_USERAGENT, "Spider/0.1 (+self-hosted)");
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, curl_error);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &t);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, progress);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, &t);
    if (post) {
        const char *body = js(rule, "body", "");
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
    }
    CURLcode code = curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r->status);
    char *v = NULL;
    curl_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &v);
    r->url = strdup(v ? v : url);
    curl_easy_getinfo(c, CURLINFO_CONTENT_TYPE, &v);
    r->content_type = strdup(v ? v : "");
    if (code == CURLE_OK && r->status >= 300 && r->status < 400) {
        curl_easy_getinfo(c, CURLINFO_REDIRECT_URL, &v);
        if (v)
            *redirect = url_resolve(url, v);
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(c);
    if (code != CURLE_OK)
        return fail(e, n,
                    t.too_large                         ? "Response exceeds max_bytes"
                    : t.io_error                        ? "File write failed"
                    : code == CURLE_ABORTED_BY_CALLBACK ? "Cancelled"
                                                        : "HTTP transfer failed: %s",
                    curl_error[0] ? curl_error : curl_easy_strerror(code));
    if (*redirect)
        return 1;
    if (r->status < 200 || r->status >= 300)
        return fail(e, n, "HTTP status %ld", r->status);
    if (!file && !r->body)
        r->body = strdup("");
    if (!file && memchr(r->body, 0, r->size))
        return fail(e, n, "Response contains binary data; use a download field");
    return 0;
}
int download_fetch(json_object *rule, const char *url, FILE *file, cancel_fn cancel, void *context,
                   HttpSession *session, HttpResponse *r, char *e, size_t n)
{
    memset(r, 0, sizeof(*r));
    char *current = strdup(url);
    if (!current)
        return fail(e, n, "Out of memory");
    bool post = !file && !strcmp(js(rule, "method", "GET"), "POST");
    int64_t deadline = now_ms() + (int64_t)ji(rule, "timeout_seconds", 30) * 1000;
    int rc = -1;
    for (int hop = 0; hop <= 5; hop++) {
        bool same_origin = url_same_origin(js(rule, "url", url), current) &&
                           url_same_origin(js(rule, "body_origin", js(rule, "url", url)), current);
        bool same_headers_origin =
            url_same_origin(js(rule, "headers_origin", js(rule, "url", url)), current);
        if (post && !same_origin) {
            rc = fail(e, n, "Cross-origin POST body forwarding is blocked");
            break;
        }
        long remaining = (long)(deadline - now_ms());
        if (remaining <= 0) {
            rc = fail(e, n, "HTTP transfer timed out");
            break;
        }
        char *redirect = NULL;
        rc = perform_once(rule, current, file, cancel, context, r, &redirect, post,
                          same_headers_origin, remaining, session, e, n);
        if (rc != 1) {
            free(redirect);
            break;
        }
        if (r->status == 301 || r->status == 302 || r->status == 303)
            post = false;
        free(current);
        current = redirect;
        download_response_free(r);
        if (file && (fflush(file) || ftruncate(fileno(file), 0) || fseek(file, 0, SEEK_SET))) {
            rc = fail(e, n, "Cannot reset redirected download");
            break;
        }
        if (hop == 5)
            rc = fail(e, n, "Too many redirects");
    }
    free(current);

    return rc;
}
