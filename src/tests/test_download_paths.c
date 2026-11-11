/* Custom download destinations: absolute root, relative path, directory
 * creation, collision policy and rollback. */
#include "crawler/crawler.h"
#include "download/download.h"
#include "storage/storage.h"
#include <arpa/inet.h>
#include <assert.h>
#include <curl/curl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    int listener;
    unsigned short port;
    const char *body;
    int accept_count;
    pthread_t thread;
} Fixture;

static int record_calls;
static int record_fail;

static int64_t record(void *ctx, const char *url, const char *path, const char *hash, int64_t bytes)
{
    (void)ctx;
    (void)url;
    (void)path;
    (void)hash;
    (void)bytes;
    record_calls++;
    return record_fail ? -1 : record_calls;
}

static void *serve(void *data)
{
    Fixture *f = data;
    for (int i = 0; i < f->accept_count; i++) {
        int fd = accept(f->listener, NULL, NULL);
        assert(fd >= 0);
        char request[8192];
        size_t used = 0;
        for (;;) {
            ssize_t count = recv(fd, request + used, sizeof(request) - used - 1, 0);
            assert(count > 0);
            used += (size_t)count;
            request[used] = 0;
            if (strstr(request, "\r\n\r\n"))
                break;
            assert(used + 1 < sizeof(request));
        }
        char header[256];
        size_t body_len = strlen(f->body);
        int n = snprintf(header, sizeof(header),
                         "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                         body_len);
        assert(n > 0 && (size_t)n < sizeof(header));
        assert(send(fd, header, (size_t)n, 0) == n);
        size_t sent = 0;
        while (sent < body_len) {
            ssize_t count = send(fd, f->body + sent, body_len - sent, 0);
            assert(count > 0);
            sent += (size_t)count;
        }
        close(fd);
    }
    close(f->listener);
    f->listener = -1;
    return NULL;
}

static void start_server(Fixture *f, const char *body, int accept_count)
{
    memset(f, 0, sizeof(*f));
    f->body = body;
    f->accept_count = accept_count;
    f->listener = socket(AF_INET, SOCK_STREAM, 0);
    assert(f->listener >= 0);
    int one = 1;
    assert(setsockopt(f->listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == 0);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(bind(f->listener, (struct sockaddr *)&address, sizeof(address)) == 0);
    assert(listen(f->listener, accept_count > 0 ? accept_count : 1) == 0);
    socklen_t size = sizeof(address);
    assert(getsockname(f->listener, (struct sockaddr *)&address, &size) == 0);
    f->port = ntohs(address.sin_port);
    assert(pthread_create(&f->thread, NULL, serve, f) == 0);
}

static void join_server(Fixture *f)
{
    assert(pthread_join(f->thread, NULL) == 0);
}

static void make_root(char *root, size_t cap)
{
    char dir[64];
    snprintf(dir, sizeof(dir), "/tmp/spider-dlp-XXXXXX");
    assert(mkdtemp(dir));
    snprintf(root, cap, "%s/music", dir);
}

static int save(DownloadClient *client, const char *options_text, const char *url, DownloadFile *file,
                char *error, size_t cap)
{
    json_object *options = json_tokener_parse(options_text);
    assert(options);
    DownloadRequest request = {.group_id = 7, .options = options, .url = url, .record = record};
    int rc = download_save(client, &request, file, error, cap);
    json_object_put(options);
    return rc;
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    assert(f);
    static char buffer[256];
    size_t count = fread(buffer, 1, sizeof(buffer) - 1, f);
    fclose(f);
    buffer[count] = 0;
    return buffer;
}

static void expect_error(const char *options, const char *needle)
{
    char root[256], error[SP_ERROR] = {0};
    make_root(root, sizeof(root));
    DownloadClient *client = download_create(root, error, sizeof(error));
    assert(client);
    DownloadFile file = {0};
    assert(save(client, options, "http://127.0.0.1:1/track.mp3", &file, error, sizeof(error)) < 0);
    assert(strstr(error, needle));
    download_destroy(client);
}

static void test_absolute_root_and_nested_path(void)
{
    char root[256], url[128], options[1024], expected[512], error[SP_ERROR] = {0};
    make_root(root, sizeof(root));
    Fixture fixture;
    start_server(&fixture, "first", 1);
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/track.mp3", fixture.port);
    snprintf(options, sizeof(options),
             "{\"download_root\":\"%s\",\"path\":\"华语/邓丽君/甜蜜蜜.mp3\"}", root);
    DownloadClient *client = download_create(root, error, sizeof(error));
    assert(client);
    DownloadFile file = {0};
    assert(save(client, options, url, &file, error, sizeof(error)) == 0);
    join_server(&fixture);
    snprintf(expected, sizeof(expected), "%s/华语/邓丽君/甜蜜蜜.mp3", root);
    assert(!strcmp(file.path, expected));
    assert(file.bytes == 5);
    assert(file.http_status == 200);
    assert(!strcmp(read_file(expected), "first"));
    download_destroy(client);
}

static void test_relative_root_rejected(void)
{
    expect_error("{\"download_root\":\"relative\",\"path\":\"a.mp3\"}", "absolute");
}

static void test_absolute_path_rejected(void)
{
    expect_error("{\"download_root\":\"/tmp\",\"path\":\"/etc/passwd\"}", "relative");
}

static void test_parent_traversal_rejected(void)
{
    expect_error("{\"download_root\":\"/tmp\",\"path\":\"a/../../b.mp3\"}", "..");
}

static void test_dot_and_empty_segments_rejected(void)
{
    expect_error("{\"download_root\":\"/tmp\",\"path\":\"./a.mp3\"}", ".");
    expect_error("{\"download_root\":\"/tmp\",\"path\":\"a//b.mp3\"}", "empty");
}

static void test_control_characters_sanitized(void)
{
    char root[256], url[128], options[1024], expected[512], error[SP_ERROR] = {0};
    make_root(root, sizeof(root));
    Fixture fixture;
    start_server(&fixture, "body", 1);
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/track.mp3", fixture.port);
    snprintf(options, sizeof(options),
             "{\"download_root\":\"%s\",\"path\":\"a\\u000ab/c.mp3\"}", root);
    DownloadClient *client = download_create(root, error, sizeof(error));
    assert(client);
    DownloadFile file = {0};
    assert(save(client, options, url, &file, error, sizeof(error)) == 0);
    join_server(&fixture);
    snprintf(expected, sizeof(expected), "%s/a_b/c.mp3", root);
    assert(!strcmp(file.path, expected));
    assert(!strcmp(read_file(expected), "body"));
    download_destroy(client);
}

static void test_legacy_default_path(void)
{
    char dir[64], url[128], error[SP_ERROR] = {0};
    snprintf(dir, sizeof(dir), "/tmp/spider-dlp-XXXXXX");
    assert(mkdtemp(dir));
    Fixture fixture;
    start_server(&fixture, "legacy", 1);
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/track.mp3", fixture.port);
    DownloadClient *client = download_create(dir, error, sizeof(error));
    assert(client);
    DownloadFile file = {0};
    assert(save(client, "{}", url, &file, error, sizeof(error)) == 0);
    join_server(&fixture);
    assert(!strncmp(file.path, dir, strlen(dir)));
    const char *base = strrchr(file.path, '/');
    assert(base);
    base++;
    assert(!strncmp(base, "7-", 2));
    assert(strlen(base) == 2 + 64 + 4);
    assert(!strcmp(base + 66, ".bin"));
    assert(!strcmp(read_file(file.path), "legacy"));
    download_destroy(client);
}

static void test_on_exists_skip_reuses_existing(void)
{
    char root[256], url[128], options[1024], expected[512], error[SP_ERROR] = {0};
    make_root(root, sizeof(root));
    Fixture fixture;
    start_server(&fixture, "first", 1);
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/track.mp3", fixture.port);
    snprintf(options, sizeof(options), "{\"download_root\":\"%s\",\"path\":\"a.mp3\"}", root);
    DownloadClient *client = download_create(root, error, sizeof(error));
    assert(client);
    DownloadFile file = {0};
    assert(save(client, options, url, &file, error, sizeof(error)) == 0);
    join_server(&fixture);
    /* No listener now: a second request would fail instead of skipping. */
    DownloadFile again = {0};
    assert(save(client, options, url, &again, error, sizeof(error)) == 0);
    snprintf(expected, sizeof(expected), "%s/a.mp3", root);
    assert(!strcmp(again.path, expected));
    assert(again.bytes == 5);
    assert(again.skipped);
    assert(!strcmp(again.sha256, file.sha256));
    assert(!strcmp(read_file(expected), "first"));
    download_destroy(client);
}

static void test_on_exists_unique_suffix(void)
{
    char root[256], url[128], options[1024], expected[512], error[SP_ERROR] = {0};
    make_root(root, sizeof(root));
    Fixture fixture;
    start_server(&fixture, "second", 2);
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/track.mp3", fixture.port);
    snprintf(options, sizeof(options),
             "{\"download_root\":\"%s\",\"path\":\"a.mp3\",\"on_exists\":\"unique\"}", root);
    DownloadClient *client = download_create(root, error, sizeof(error));
    assert(client);
    DownloadFile first = {0}, second = {0};
    assert(save(client, options, url, &first, error, sizeof(error)) == 0);
    assert(save(client, options, url, &second, error, sizeof(error)) == 0);
    join_server(&fixture);
    snprintf(expected, sizeof(expected), "%s/a (1).mp3", root);
    assert(!strcmp(second.path, expected));
    assert(!strcmp(read_file(expected), "second"));
    download_destroy(client);
}

static void test_on_exists_overwrite(void)
{
    char root[256], url[128], options[1024], expected[512], error[SP_ERROR] = {0};
    make_root(root, sizeof(root));
    Fixture fixture;
    start_server(&fixture, "second", 2);
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/track.mp3", fixture.port);
    snprintf(options, sizeof(options),
             "{\"download_root\":\"%s\",\"path\":\"a.mp3\",\"on_exists\":\"overwrite\"}", root);
    DownloadClient *client = download_create(root, error, sizeof(error));
    assert(client);
    DownloadFile first = {0}, second = {0};
    assert(save(client, options, url, &first, error, sizeof(error)) == 0);
    assert(save(client, options, url, &second, error, sizeof(error)) == 0);
    join_server(&fixture);
    snprintf(expected, sizeof(expected), "%s/a.mp3", root);
    assert(!strcmp(second.path, expected));
    assert(!strcmp(read_file(expected), "second"));
    download_destroy(client);
}

static void test_record_failure_removes_new_file(void)
{
    char root[256], url[128], options[1024], expected[512], error[SP_ERROR] = {0};
    make_root(root, sizeof(root));
    Fixture fixture;
    start_server(&fixture, "body", 1);
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/track.mp3", fixture.port);
    snprintf(options, sizeof(options), "{\"download_root\":\"%s\",\"path\":\"a.mp3\"}", root);
    DownloadClient *client = download_create(root, error, sizeof(error));
    assert(client);
    DownloadFile file = {0};
    record_fail = 1;
    assert(save(client, options, url, &file, error, sizeof(error)) < 0);
    record_fail = 0;
    join_server(&fixture);
    snprintf(expected, sizeof(expected), "%s/a.mp3", root);
    assert(access(expected, F_OK) != 0);
    download_destroy(client);
}

static void test_spider_config_root_injected(void)
{
    char dir[64], db[256], root[256], url[128], config_text[1024], expected[512],
        error[SP_ERROR] = {0};
    snprintf(dir, sizeof(dir), "/tmp/spider-dlp-XXXXXX");
    assert(mkdtemp(dir));
    snprintf(db, sizeof(db), "%s/spider.db", dir);
    snprintf(root, sizeof(root), "%s/music", dir);
    snprintf(config_text, sizeof(config_text), "{\"name\":\"cfg\",\"download_root\":\"%s\"}", root);
    Store *store = store_create(db, error, sizeof(error));
    assert(store);
    json_object *config = json_tokener_parse(config_text);
    assert(config);
    int64_t spider = store_save_spider(store, 0, config, error, sizeof(error));
    assert(spider > 0);
    int64_t run = store_start_run(store, spider, error, sizeof(error));
    assert(run > 0);
    json_object *claimed = store_claim_run(store, NULL, 0, error, sizeof(error));
    assert(claimed && json_object_get_int64(jg(claimed, "id")) == run);
    json_object_put(claimed);
    Fixture fixture;
    start_server(&fixture, "configured", 1);
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/track.mp3", fixture.port);
    DownloadClient *client = download_create(dir, error, sizeof(error));
    assert(client);
    CrawlerContext context = {
        .run_id = run, .config = config, .store = store, .downloads = client};
    json_object *options = json_tokener_parse("{\"path\":\"a.mp3\"}");
    assert(options);
    DownloadFile file = {0};
    assert(crawler_save_file(&context, options, url, &file, error, sizeof(error)) == 0);
    join_server(&fixture);
    snprintf(expected, sizeof(expected), "%s/a.mp3", root);
    assert(!strcmp(file.path, expected));
    assert(!strcmp(read_file(expected), "configured"));
    json_object_put(options);
    download_destroy(client);
    json_object_put(config);
    store_destroy(store);
}

static void test_file_larger_than_fetch_default(void)
{
    char root[256], url[128], options[1024], expected[512], error[SP_ERROR] = {0};
    make_root(root, sizeof(root));
    size_t size = 5 * 1024 * 1024;
    char *body = malloc(size + 1);
    assert(body);
    memset(body, 'x', size);
    body[size] = 0;
    Fixture fixture;
    start_server(&fixture, body, 1);
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/big.bin", fixture.port);
    snprintf(options, sizeof(options), "{\"download_root\":\"%s\",\"path\":\"big.bin\"}", root);
    DownloadClient *client = download_create(root, error, sizeof(error));
    assert(client);
    DownloadFile file = {0};
    assert(save(client, options, url, &file, error, sizeof(error)) == 0);
    join_server(&fixture);
    assert(file.bytes == (int64_t)size);
    snprintf(expected, sizeof(expected), "%s/big.bin", root);
    struct stat st;
    assert(stat(expected, &st) == 0 && st.st_size == (off_t)size);
    free(body);
    download_destroy(client);
}

int main(void)
{
    setenv("NO_PROXY", "127.0.0.1", 1);
    setenv("no_proxy", "127.0.0.1", 1);
    assert(!curl_global_init(CURL_GLOBAL_DEFAULT));
    test_absolute_root_and_nested_path();
    test_file_larger_than_fetch_default();
    test_relative_root_rejected();
    test_absolute_path_rejected();
    test_parent_traversal_rejected();
    test_dot_and_empty_segments_rejected();
    test_control_characters_sanitized();
    test_legacy_default_path();
    test_on_exists_skip_reuses_existing();
    test_on_exists_unique_suffix();
    test_on_exists_overwrite();
    test_record_failure_removes_new_file();
    test_spider_config_root_injected();
    curl_global_cleanup();
    puts("download paths passed");
    return 0;
}
