/* End-to-end archive.org scan and download against a local HTTP fixture. */
#include "crawler/crawler.h"
#include "download/download.h"
#include "spiders/archive_org/archive_org.h"
#include "storage/storage.h"
#include <arpa/inet.h>
#include <assert.h>
#include <curl/curl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
    int listener;
    unsigned short port;
    int accept_count;
    pthread_t thread;
} Fixture;

static const char *search_body =
    "{\"response\":{\"numFound\":1,\"docs\":[{\"identifier\":\"item1\",\"title\":\"华语测试"
    "专辑\",\"creator\":\"测试歌手\",\"subject\":[\"Chinese Music\"],\"collection\":[\"opensource_"
    "audio\"],\"language\":\"chi\"}]}}";

static const char *metadata_body =
    "{\"metadata\":{\"identifier\":\"item1\",\"title\":\"华语测试专辑\",\"creator\":\"测试歌手\","
    "\"subject\":[\"Chinese Music\"],\"collection\":[\"opensource_audio\"],\"language\":\"chi\","
    "\"description\":\"华语音乐\",\"server\":\"127.0.0.1\",\"dir\":\"/download/item1\"},"
    "\"files\":["
    "{\"name\":\"测试歌手 - 测试歌曲.mp3\",\"format\":\"VBR MP3\",\"size\":\"50000\","
    "\"length\":\"3:30\",\"sha1\":\"mp3sha\",\"original\":\"测试歌手 - 测试歌曲.flac\"},"
    "{\"name\":\"测试歌手 - 测试歌曲.flac\",\"format\":\"24bit Flac\",\"size\":\"500000\","
    "\"length\":\"3:30\",\"sha1\":\"flacsha\",\"original\":\"测试歌手 - 测试歌曲.flac\"},"
    "{\"name\":\"cover.png\",\"format\":\"PNG\",\"size\":\"100\"}]}";

static const char *audio_body = "audio-bytes";

static void reply(int fd, const char *body)
{
    char header[256];
    int n = snprintf(header, sizeof(header),
                     "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                     strlen(body));
    assert(n > 0 && (size_t)n < sizeof(header));
    assert(send(fd, header, (size_t)n, 0) == n);
    assert(send(fd, body, strlen(body), 0) == (ssize_t)strlen(body));
}

static void *serve(void *data)
{
    Fixture *fixture = data;
    for (int i = 0; i < fixture->accept_count; i++) {
        int fd = accept(fixture->listener, NULL, NULL);
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
        if (!strncmp(request, "GET /advancedsearch.php", 23))
            reply(fd, search_body);
        else if (!strncmp(request, "GET /metadata/item1", 19))
            reply(fd, metadata_body);
        else if (!strncmp(request, "GET /download/item1/", 20))
            reply(fd, audio_body);
        else {
            const char *missing = "not found";
            char header[128];
            int n = snprintf(header, sizeof(header),
                             "HTTP/1.1 404 Not Found\r\nContent-Length: %zu\r\nConnection: "
                             "close\r\n\r\n",
                             strlen(missing));
            assert(n > 0 && (size_t)n < sizeof(header));
            assert(send(fd, header, (size_t)n, 0) == n);
            assert(send(fd, missing, strlen(missing), 0) == (ssize_t)strlen(missing));
        }
        close(fd);
    }
    close(fixture->listener);
    fixture->listener = -1;
    return NULL;
}

static void start_server(Fixture *fixture, int accept_count)
{
    memset(fixture, 0, sizeof(*fixture));
    fixture->accept_count = accept_count;
    fixture->listener = socket(AF_INET, SOCK_STREAM, 0);
    assert(fixture->listener >= 0);
    int one = 1;
    assert(setsockopt(fixture->listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == 0);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(bind(fixture->listener, (struct sockaddr *)&address, sizeof(address)) == 0);
    assert(listen(fixture->listener, accept_count) == 0);
    socklen_t size = sizeof(address);
    assert(getsockname(fixture->listener, (struct sockaddr *)&address, &size) == 0);
    fixture->port = ntohs(address.sin_port);
    assert(pthread_create(&fixture->thread, NULL, serve, fixture) == 0);
}

static char *db_path(char *path, size_t cap)
{
    snprintf(path, cap, "/tmp/spider-archive-XXXXXX");
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    return path;
}

static json_object *config(const char *root, unsigned short port, const char *mode)
{
    char raw[4096];
    snprintf(raw, sizeof(raw),
             "{\"name\":\"archive test\",\"spider_code\":\"test-spider\",\"mode\":\"%s\","
             "\"download_root\":\"%s\",\"site_base_url\":\"http://127.0.0.1:%u\","
             "\"search_url\":\"http://127.0.0.1:%u/advancedsearch.php\","
             "\"metadata_url\":\"http://127.0.0.1:%u/metadata/\","
             "\"download_base\":\"http://127.0.0.1:%u/download/\","
             "\"queries\":[\"mediatype:audio AND subject:mandopop\"],"
             "\"artist_whitelist\":[],\"artist_whitelist_strict\":false,"
             "\"exclude_keywords\":[\"有声书\"],\"exclude_collections\":[\"radio\"],"
             "\"format_prefer\":[\"VBR MP3\",\"MP3\",\"Flac\"],"
             "\"min_score\":3,\"min_seconds\":30,\"max_seconds\":1200,\"min_bytes\":20480,"
             "\"page_size\":100,\"max_pages\":1,\"max_items\":10,\"download_limit\":10,"
             "\"delay_ms\":0,\"max_bytes\":1048576}",
             mode, root, port, port, port, port);
    json_object *object = json_tokener_parse(raw);
    assert(object);
    return object;
}

static char *read_file(const char *path)
{
    FILE *file = fopen(path, "rb");
    assert(file);
    static char buffer[256];
    size_t count = fread(buffer, 1, sizeof(buffer) - 1, file);
    fclose(file);
    buffer[count] = 0;
    return buffer;
}

int main(void)
{
    setenv("NO_PROXY", "127.0.0.1", 1);
    setenv("no_proxy", "127.0.0.1", 1);
    assert(!curl_global_init(CURL_GLOBAL_DEFAULT));

    char path[64], root[64], error[SP_ERROR] = {0};
    db_path(path, sizeof(path));
    snprintf(root, sizeof(root), "/tmp/spider-archive-music-XXXXXX");
    assert(mkdtemp(root));
    Store *store = store_create(path, error, sizeof(error));
    assert(store);
    Fixture fixture;
    start_server(&fixture, 3);

    json_object *scan = config(root, fixture.port, "scan");
    assert(archive_org_validate(scan, error, sizeof(error)) == 0);
    int64_t spider = store_save_spider(store, 0, scan, error, sizeof(error));
    assert(spider > 0);
    int64_t run = store_start_run(store, spider, error, sizeof(error));
    assert(run > 0);
    json_object *claimed = store_claim_run(store, NULL, 0, error, sizeof(error));
    assert(claimed && json_object_get_int64(jg(claimed, "id")) == run);
    json_object_put(claimed);

    DownloadClient *downloads = download_create(root, error, sizeof(error));
    assert(downloads);
    CrawlerContext context = {.spider_id = spider,
                              .run_id = run,
                              .config = scan,
                              .store = store,
                              .downloads = downloads};
    assert(archive_org_run(&context, error, sizeof(error)) == 0);

    json_object *queue = store_download_queue(store, "test-spider", "music", 100, error,
                                              sizeof(error));
    assert(queue && json_object_array_length(queue) == 1);
    json_object *row = json_object_array_get_idx(queue, 0);
    assert(!strcmp(js(row, "artist", ""), "测试歌手"));
    assert(!strcmp(js(row, "title", ""), "测试歌曲"));
    assert(!strcmp(js(row, "decision", ""), "include"));
    char expected[512];
    snprintf(expected, sizeof(expected), "%s/测试歌手/测试歌曲.mp3", root);
    assert(!strcmp(js(row, "target_path", ""), expected));
    json_object_put(queue);

    json_object *download = config(root, fixture.port, "download");
    CrawlerContext download_context = {.spider_id = spider,
                                       .run_id = run,
                                       .config = download,
                                       .store = store,
                                       .downloads = downloads};
    assert(archive_org_run(&download_context, error, sizeof(error)) == 0);
    assert(!strcmp(read_file(expected), audio_body));
    queue = store_download_queue(store, "test-spider", "music", 100, error, sizeof(error));
    assert(queue && json_object_array_length(queue) == 0);
    json_object_put(queue);
    json_object *summary = store_download_summary(store, "test-spider", error, sizeof(error));
    assert(summary);
    bool downloaded = false;
    for (size_t i = 0; i < json_object_array_length(summary); i++) {
        json_object *entry = json_object_array_get_idx(summary, i);
        if (!strcmp(js(entry, "status", ""), "downloaded") && ji(entry, "count", 0) == 1)
            downloaded = true;
    }
    assert(downloaded);
    json_object_put(summary);

    assert(pthread_join(fixture.thread, NULL) == 0);
    download_destroy(downloads);
    json_object_put(download);
    json_object_put(scan);
    store_destroy(store);
    unlink(path);
    curl_global_cleanup();
    puts("archive.org integration passed");
    return 0;
}
