/* The public transport takes URL separately from options. Default origins must
 * use that URL, while explicit credential/body origin restrictions still win. */
#include "download/download.h"
#include <arpa/inet.h>
#include <assert.h>
#include <curl/curl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
typedef struct {
    int listener;
    char request[8192];
} Fixture;
static void *serve(void *data)
{
    Fixture *f = data;
    int fd = accept(f->listener, NULL, NULL);
    assert(fd >= 0);
    size_t used = 0;
    for (;;) {
        ssize_t count = recv(fd, f->request + used, sizeof(f->request) - used - 1, 0);
        assert(count > 0);
        used += (size_t)count;
        f->request[used] = 0;
        char *headers = strstr(f->request, "\r\n\r\n");
        if (headers) {
            size_t length = 0;
            char *cl = strstr(f->request, "Content-Length:");
            if (cl)
                length = (size_t)strtoul(cl + 15, NULL, 10);
            if (used >= (size_t)(headers + 4 - f->request) + length)
                break;
        }
        assert(used + 1 < sizeof(f->request));
    }
    const char reply[] = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}";
    assert(send(fd, reply, sizeof(reply) - 1, 0) == sizeof(reply) - 1);
    close(fd);
    return NULL;
}
static void request(const char *raw, bool expect_auth, bool expect_post)
{
    Fixture f = {.listener = socket(AF_INET, SOCK_STREAM, 0)};
    assert(f.listener >= 0);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(bind(f.listener, (struct sockaddr *)&address, sizeof(address)) == 0);
    assert(listen(f.listener, 1) == 0);
    socklen_t size = sizeof(address);
    assert(getsockname(f.listener, (struct sockaddr *)&address, &size) == 0);
    pthread_t thread;
    assert(pthread_create(&thread, NULL, serve, &f) == 0);
    char url[128], e[SP_ERROR] = {0};
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/resource", ntohs(address.sin_port));
    json_object *options = json_tokener_parse(raw);
    HttpResponse response = {0};
    assert(!download_fetch(options, url, NULL, NULL, NULL, NULL, &response, e, sizeof(e)));
    assert(response.status == 200);
    download_response_free(&response);
    json_object_put(options);
    pthread_join(thread, NULL);
    close(f.listener);
    assert((strstr(f.request, "Authorization: Bearer native") != NULL) == expect_auth);
    if (expect_post) {
        assert(!strncmp(f.request, "POST ", 5));
        assert(strstr(f.request, "\r\n\r\nnative-body"));
    } else
        assert(!strncmp(f.request, "GET ", 4));
}
int main(void)
{
    setenv("NO_PROXY", "127.0.0.1", 1);
    setenv("no_proxy", "127.0.0.1", 1);
    assert(!curl_global_init(CURL_GLOBAL_DEFAULT));
    request("{\"headers\":{\"Authorization\":\"Bearer native\"}}", true, false);
    request("{\"method\":\"POST\",\"body\":\"native-body\"}", false, true);
    request("{\"headers\":{\"Authorization\":\"Bearer "
            "native\"},\"headers_origin\":\"https://another-origin.invalid\"}",
            false, false);
    json_object *options = json_tokener_parse(
        "{\"method\":\"POST\",\"body\":\"secret\",\"body_origin\":\"http://127.0.0.1:2\"}");
    HttpResponse response = {0};
    char e[SP_ERROR] = {0};
    assert(download_fetch(options, "http://127.0.0.1:1/", NULL, NULL, NULL, NULL, &response, e,
                          sizeof(e)) < 0);
    assert(strstr(e, "Cross-origin POST"));
    download_response_free(&response);
    json_object_put(options);
    curl_global_cleanup();
    puts("public download transport passed");
    return 0;
}
