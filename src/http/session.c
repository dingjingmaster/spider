#include "session.h"
#include <stdlib.h>
#include <string.h>
struct HttpSession {
    int64_t run;
    Store *store;
    CURLSH *share;
    pthread_mutex_t cookie_mutex, save_mutex;
    unsigned refs;
    struct HttpSession *next;
};
struct SessionPool {
    Store *store;
    pthread_mutex_t mutex;
    HttpSession *head;
};
static void cookie_lock(CURL *c, curl_lock_data data, curl_lock_access access, void *ctx)
{
    (void)c;
    (void)data;
    (void)access;
    pthread_mutex_lock(&((HttpSession *)ctx)->cookie_mutex);
}
static void cookie_unlock(CURL *c, curl_lock_data data, void *ctx)
{
    (void)c;
    (void)data;
    pthread_mutex_unlock(&((HttpSession *)ctx)->cookie_mutex);
}
static HttpSession *create_session(Store *store, int64_t run, char *e, size_t n)
{
    HttpSession *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->store = store;
    s->run = run;
    pthread_mutex_init(&s->cookie_mutex, NULL);
    pthread_mutex_init(&s->save_mutex, NULL);
    s->share = curl_share_init();
    if (!s->share) {
        session_destroy(s);
        return NULL;
    }
    curl_share_setopt(s->share, CURLSHOPT_LOCKFUNC, cookie_lock);
    curl_share_setopt(s->share, CURLSHOPT_UNLOCKFUNC, cookie_unlock);
    curl_share_setopt(s->share, CURLSHOPT_USERDATA, s);
    curl_share_setopt(s->share, CURLSHOPT_SHARE, CURL_LOCK_DATA_COOKIE);
    if (store) {
        json_object *rows =
            store_query(store, "SELECT cookies FROM sessions WHERE run_id=?", run, e, n);
        if (!rows) {
            session_destroy(s);
            return NULL;
        }
        if (json_object_array_length(rows)) {
            json_object *cookies =
                json_tokener_parse(js(json_object_array_get_idx(rows, 0), "cookies", "[]"));
            CURL *c = curl_easy_init();
            if (!c) {
                json_object_put(rows);
                json_object_put(cookies);
                session_destroy(s);
                return NULL;
            }
            session_attach(s, c);
            if (cookies && json_object_is_type(cookies, json_type_array))
                for (size_t i = 0; i < json_object_array_length(cookies); i++)
                    curl_easy_setopt(c, CURLOPT_COOKIELIST,
                                     json_object_get_string(json_object_array_get_idx(cookies, i)));
            curl_easy_cleanup(c);
            json_object_put(cookies);
        }
        json_object_put(rows);
    }
    return s;
}
void session_attach(HttpSession *s, CURL *c)
{
    if (s) {
        curl_easy_setopt(c, CURLOPT_SHARE, s->share);
        curl_easy_setopt(c, CURLOPT_COOKIEFILE, "");
    }
}
HttpSession *session_temporary(void)
{
    return create_session(NULL, 0, NULL, 0);
}
void session_destroy(HttpSession *s)
{
    if (!s)
        return;
    if (s->share)
        curl_share_cleanup(s->share);
    pthread_mutex_destroy(&s->cookie_mutex);
    pthread_mutex_destroy(&s->save_mutex);
    free(s);
}
SessionPool *session_pool_create(Store *store)
{
    SessionPool *p = calloc(1, sizeof(*p));
    if (p) {
        p->store = store;
        pthread_mutex_init(&p->mutex, NULL);
    }
    return p;
}
void session_pool_destroy(SessionPool *p)
{
    if (!p)
        return;
    HttpSession *s = p->head;
    while (s) {
        HttpSession *next = s->next;
        session_destroy(s);
        s = next;
    }
    pthread_mutex_destroy(&p->mutex);
    free(p);
}
HttpSession *session_acquire(SessionPool *p, int64_t run, char *e, size_t n)
{
    pthread_mutex_lock(&p->mutex);
    HttpSession *s = p->head;
    while (s && s->run != run)
        s = s->next;
    if (!s) {
        s = create_session(p->store, run, e, n);
        if (s) {
            s->next = p->head;
            p->head = s;
        }
    }
    if (s)
        s->refs++;
    pthread_mutex_unlock(&p->mutex);
    return s;
}
void session_release(SessionPool *p, HttpSession *s)
{
    if (!s)
        return;
    pthread_mutex_lock(&p->mutex);
    if (s->refs)
        s->refs--;
    pthread_mutex_unlock(&p->mutex);
}
void session_pool_prune(SessionPool *p, int64_t run)
{
    if (!p)
        return;
    pthread_mutex_lock(&p->mutex);
    HttpSession **link = &p->head;
    while (*link && (*link)->run != run)
        link = &(*link)->next;
    {
        json_object *rows =
            store_query(p->store, "SELECT status FROM runs WHERE id=?", run, NULL, 0);
        if (rows && json_object_array_length(rows)) {
            const char *status = js(json_object_array_get_idx(rows, 0), "status", "");
            if (strcmp(status, "queued") && strcmp(status, "running")) {
                if (*link && !(*link)->refs) {
                    HttpSession *old = *link;
                    *link = old->next;
                    session_destroy(old);
                }
                pthread_mutex_lock(&p->store->mutex);
                sqlite3_stmt *q = NULL;
                if (sqlite3_prepare_v2(
                        p->store->db,
                        "DELETE FROM sessions WHERE run_id=? AND EXISTS(SELECT 1 FROM runs WHERE "
                        "id=? AND status NOT IN('queued','running'))",
                        -1, &q, NULL) == SQLITE_OK) {
                    sqlite3_bind_int64(q, 1, run);
                    sqlite3_bind_int64(q, 2, run);
                    sqlite3_step(q);
                }
                sqlite3_finalize(q);
                pthread_mutex_unlock(&p->store->mutex);
            }
        }
        json_object_put(rows);
    }
    pthread_mutex_unlock(&p->mutex);
}
int session_persist(HttpSession *s, char *e, size_t n)
{
    if (!s || !s->store)
        return 0;
    pthread_mutex_lock(&s->save_mutex);
    CURL *c = curl_easy_init();
    if (!c) {
        pthread_mutex_unlock(&s->save_mutex);
        return fail(e, n, "Cannot save HTTP session");
    }
    session_attach(s, c);
    struct curl_slist *cookies = NULL;
    CURLcode code = curl_easy_getinfo(c, CURLINFO_COOKIELIST, &cookies);
    json_object *list = json_object_new_array();
    size_t bytes = 0;
    for (struct curl_slist *p = cookies; p; p = p->next) {
        bytes += strlen(p->data);
        json_object_array_add(list, json_object_new_string(p->data));
    }
    curl_slist_free_all(cookies);
    curl_easy_cleanup(c);
    int rc = -1;
    if (code == CURLE_OK && bytes <= 1048576) {
        pthread_mutex_lock(&s->store->mutex);
        sqlite3_stmt *q = NULL;
        if (sqlite3_prepare_v2(
                s->store->db,
                "INSERT INTO sessions(run_id,cookies) SELECT ?,? WHERE EXISTS(SELECT 1 FROM runs "
                "WHERE id=? AND status='running' AND cancel_requested=0) ON "
                "CONFLICT(run_id) DO UPDATE SET cookies=excluded.cookies",
                -1, &q, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(q, 1, s->run);
            sqlite3_bind_text(q, 2, json_object_to_json_string_ext(list, JSON_C_TO_STRING_PLAIN),
                              -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(q, 3, s->run);
            rc = sqlite3_step(q) == SQLITE_DONE && sqlite3_changes(s->store->db) ? 0 : -1;
        }
        sqlite3_finalize(q);
        pthread_mutex_unlock(&s->store->mutex);
    }
    json_object_put(list);
    pthread_mutex_unlock(&s->save_mutex);
    return rc ? fail(e, n, "Cannot persist HTTP session (limit 1 MiB)") : 0;
}
