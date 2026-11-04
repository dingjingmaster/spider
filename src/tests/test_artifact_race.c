/* Force registration failure after another worker publishes identical content.
 * Only the failing storage call is injected; HTTP, files and successful SQLite
 * registration are real. This pins the publication/rollback ownership boundary. */
#include "artifact/artifact.h"
#include "storage/workflow.h"
#include <assert.h>
#include <arpa/inet.h>
#include <curl/curl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <time.h>

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static bool bad_published, good_registered;
int64_t __real_store_add_artifact(Store *, int64_t, const char *, const char *, const char *,
                                  int64_t);
int64_t __wrap_store_add_artifact(Store *s, int64_t run, const char *url, const char *path,
                                  const char *hash, int64_t bytes)
{
    pthread_mutex_lock(&mutex);
    if (strstr(url, "/bad")) {
        bad_published = true;
        pthread_cond_broadcast(&changed);
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec++;
        while (!good_registered)
            if (pthread_cond_timedwait(&changed, &mutex, &deadline))
                break;
        pthread_mutex_unlock(&mutex);
        return -1;
    }
    while (!bad_published)
        pthread_cond_wait(&changed, &mutex);
    pthread_mutex_unlock(&mutex);
    int64_t id = __real_store_add_artifact(s, run, url, path, hash, bytes);
    pthread_mutex_lock(&mutex);
    good_registered = true;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&mutex);
    return id;
}
static void *server(void *arg)
{
    int listen_fd = *(int *)arg;
    for (int i = 0; i < 2; i++) {
        int fd = accept(listen_fd, NULL, NULL);
        assert(fd >= 0);
        char request[4096];
        assert(recv(fd, request, sizeof(request), 0) > 0);
        const char response[] =
            "HTTP/1.1 200 OK\r\nContent-Length: 7\r\nConnection: close\r\n\r\npayload";
        assert(send(fd, response, sizeof(response) - 1, 0) == sizeof(response) - 1);
        close(fd);
    }
    return NULL;
}
typedef struct {
    App *app;
    int64_t run;
    char url[128];
    int result;
} Download;
static void *download(void *arg)
{
    Download *d = arg;
    char e[512] = {0};
    json_object *rule = json_object_new_object();
    d->result = artifact_download(d->app, d->run, rule, d->url, NULL, NULL, e, sizeof(e));
    json_object_put(rule);
    return NULL;
}
int main(void)
{
    setenv("NO_PROXY", "127.0.0.1", 1);
    setenv("no_proxy", "127.0.0.1", 1);
    assert(curl_global_init(CURL_GLOBAL_DEFAULT) == 0);
    App app = {0};
    pthread_mutex_init(&app.artifact_mutex, NULL);
    char directory[] = "/tmp/spider-artifact-XXXXXX", e[512] = {0};
    assert(mkdtemp(directory));
    strcpy(app.data_dir, directory);
    char downloads[4096];
    snprintf(downloads, sizeof(downloads), "%s/downloads", directory);
    assert(mkdirs(downloads) == 0);
    assert(store_open(&app.store, ":memory:", e, sizeof(e)) == 0);
    json_object *rule =
        json_tokener_parse("{\"schema_version\":2,\"name\":\"race\",\"entry\":\"save\",\"steps\":["
                           "{\"id\":\"save\",\"type\":\"emit\",\"fields\":{}}]}");
    int64_t sid = store_save_spider(&app.store, 0, rule, e, sizeof(e));
    json_object_put(rule);
    int64_t run = store_start_run(&app.store, sid, e, sizeof(e));
    assert(run > 0);
    assert(wf_initialize(&app.store, e, sizeof(e)) == 0);
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    assert(listener >= 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(bind(listener, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    assert(listen(listener, 4) == 0);
    socklen_t length = sizeof(addr);
    assert(getsockname(listener, (struct sockaddr *)&addr, &length) == 0);
    Download bad = {.app = &app, .run = run}, good = {.app = &app, .run = run};
    snprintf(bad.url, sizeof(bad.url), "http://127.0.0.1:%u/bad", ntohs(addr.sin_port));
    snprintf(good.url, sizeof(good.url), "http://127.0.0.1:%u/good", ntohs(addr.sin_port));
    pthread_t serving, a, b;
    assert(pthread_create(&serving, NULL, server, &listener) == 0);
    assert(pthread_create(&a, NULL, download, &bad) == 0);
    pthread_mutex_lock(&mutex);
    while (!bad_published)
        pthread_cond_wait(&changed, &mutex);
    pthread_mutex_unlock(&mutex);
    assert(pthread_create(&b, NULL, download, &good) == 0);
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    pthread_join(serving, NULL);
    close(listener);
    assert(bad.result < 0 && good.result == 0);
    json_object *rows =
        store_query(&app.store, "SELECT path FROM artifacts WHERE run_id=?", run, e, sizeof(e));
    assert(rows && json_object_array_length(rows) == 1);
    const char *path = js(json_object_array_get_idx(rows, 0), "path", "");
    assert(access(path, R_OK) == 0);
    unlink(path);
    json_object_put(rows);
    store_close(&app.store);
    rmdir(downloads);
    rmdir(directory);
    curl_global_cleanup();
    puts("artifact rollback race passed");
    return 0;
}
