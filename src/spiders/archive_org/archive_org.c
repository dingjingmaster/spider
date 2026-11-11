#include "archive_org.h"
#include "private/filter.h"
#include "storage/storage.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARCHIVE_DEFAULT_CODE "archive-org-chinese-music"

/* ===== 可修改的默认配置 =====
 * 只改这一段：下载根目录、模式、是否启用、查询和过滤规则。
 * mode=scan 先只扫描入库；确认清单后改成 download 再重新编译启动。
 * enabled=true 时按 interval_seconds 自动调度；false 时仍可在网页点“运行”手动触发。
 * interval_seconds 必须大于 0 才会自动调度；download_root 必须是绝对路径。
 */
static const char *ARCHIVE_DEFAULT_CONFIG =
    "{"
    "\"name\":\"archive.org 华语音乐\","
    "\"spider_code\":\"" ARCHIVE_DEFAULT_CODE "\","
    "\"mode\":\"scan\","
    "\"download_root\":\"/backup/data/music\","
    "\"site_base_url\":\"https://archive.org\","
    "\"search_url\":\"https://archive.org/advancedsearch.php\","
    "\"metadata_url\":\"https://archive.org/metadata/\","
    "\"download_base\":\"https://archive.org/download/\","
    "\"queries\":["
    "\"mediatype:audio AND (subject:mandopop OR subject:cantopop OR subject:cpop OR "
    "subject:\\\"Chinese pop\\\" OR subject:\\\"Chinese music\\\" OR subject:\\\"Chinese "
    "classical\\\")\","
    "\"identifier:music-of-china*\""
    "],"
    "\"artist_whitelist\":[],"
    "\"artist_whitelist_strict\":false,"
    "\"exclude_keywords\":[\"有声书\",\"评书\",\"相声\",\"讲座\",\"课程\",\"电台\",\"广播\","
    "\"朗读\",\"朗诵\",\"英语\",\"日语\",\"韩语\",\"睡眠\",\"白噪音\",\"ASMR\",\"儿童故事\","
    "\"新闻\",\"访谈\",\"播客\",\"音效\",\"铃声\"],"
    "\"exclude_collections\":[\"radio\",\"radioprograms\",\"podcasts\",\"librivoxaudio\","
    "\"stream_only\",\"audio_bookspoetry\"],"
    "\"format_prefer\":[\"24bit Flac\",\"Flac\",\"320Kbps MP3\",\"VBR MP3\",\"128Kbps MP3\","
    "\"MP3\",\"Ogg Vorbis\",\"WAVE\",\"Apple Lossless\"],"
    "\"min_score\":3,\"min_seconds\":30,\"max_seconds\":1200,\"min_bytes\":20480,"
    "\"page_size\":500,\"max_pages\":50,\"max_items\":0,\"download_limit\":100,"
    "\"delay_ms\":700,\"max_bytes\":33554432,"
    "\"interval_seconds\":86400,\"enabled\":true"
    "}";

typedef struct {
    char **items;
    size_t count, capacity;
} Seen;

static char *encode(const char *text)
{
    CURL *curl = curl_easy_init();
    if (!curl)
        return NULL;
    char *escaped = curl_easy_escape(curl, text ? text : "", 0);
    curl_easy_cleanup(curl);
    if (!escaped)
        return NULL;
    char *copy = strdup(escaped);
    curl_free(escaped);
    return copy;
}

static char *encode_path(const char *path)
{
    size_t capacity = strlen(path) * 3 + 4, used = 0;
    char *out = malloc(capacity);
    if (!out)
        return NULL;
    const char *p = path;
    while (*p) {
        const char *slash = strchr(p, '/');
        size_t length = slash ? (size_t)(slash - p) : strlen(p);
        char *part = strndup(p, length);
        char *encoded = part ? encode(part) : NULL;
        free(part);
        if (!encoded) {
            free(out);
            return NULL;
        }
        size_t need = used + strlen(encoded) + (slash ? 2 : 1);
        if (need > capacity) {
            capacity = need * 2;
            char *grown = realloc(out, capacity);
            if (!grown) {
                free(encoded);
                free(out);
                return NULL;
            }
            out = grown;
        }
        memcpy(out + used, encoded, strlen(encoded));
        used += strlen(encoded);
        free(encoded);
        if (slash)
            out[used++] = '/';
        p = slash ? slash + 1 : p + length;
    }
    out[used] = 0;
    return out;
}

static char *join_url(const char *base, const char *part)
{
    size_t base_len = strlen(base), part_len = strlen(part);
    bool slash = base_len && base[base_len - 1] != '/';
    char *out = malloc(base_len + (slash ? 1 : 0) + part_len + 1);
    if (!out)
        return NULL;
    memcpy(out, base, base_len);
    size_t used = base_len;
    if (slash)
        out[used++] = '/';
    memcpy(out + used, part, part_len + 1);
    return out;
}

static char *build_search_url(const char *base, const char *query, int rows, int page)
{
    char *encoded = encode(query);
    if (!encoded)
        return NULL;
    char *out = NULL;
    size_t size = 0;
    FILE *stream = open_memstream(&out, &size);
    if (!stream) {
        free(encoded);
        return NULL;
    }
    fprintf(stream,
            "%s%cq=%s&fl[]=identifier&fl[]=title&fl[]=creator&fl[]=subject&fl[]=collection&"
            "fl[]=language&rows=%d&page=%d&output=json",
            base, strchr(base, '?') ? '&' : '?', encoded, rows, page);
    fclose(stream);
    free(encoded);
    return out;
}

static void copy_field(json_object *target, json_object *source, const char *key)
{
    json_object *value = jg(source, key);
    if (value)
        json_object_object_add(target, key, json_object_get(value));
}

static json_object *fetch_json(CrawlerContext *context, json_object *config, const char *url,
                               char *error, size_t cap)
{
    json_object *options = json_object_new_object();
    json_object_object_add(options, "max_bytes",
                           json_object_new_int(ji(config, "max_bytes", 33554432)));
    json_object_object_add(options, "timeout_seconds", json_object_new_int(60));
    json_object *headers = json_object_new_object();
    json_object_object_add(headers, "User-Agent", json_object_new_string("spider/1.0 archive-org"));
    json_object_object_add(options, "headers", headers);
    int delay = ji(config, "delay_ms", 700);
    json_object *result = NULL;
    for (int attempt = 0; attempt < 3 && !result; attempt++) {
        if (crawler_cancelled(context)) {
            fail(error, cap, "Cancelled");
            break;
        }
        HttpResponse response = {0};
        char local[SP_ERROR] = {0};
        int rc = crawler_fetch(context, options, url, &response, local, sizeof(local));
        if (rc == 0 && response.status == 200 && response.body)
            result = json_parse(response.body, local, sizeof(local));
        else if (rc == 0)
            fail(local, sizeof(local), "HTTP %ld for %s", response.status, url);
        download_response_free(&response);
        if (result)
            break;
        fail(error, cap, "%s", local);
        if (attempt < 2)
            sleep_ms(delay * (attempt + 1));
    }
    json_object_put(options);
    return result;
}

static bool seen_has(Seen *seen, const char *identifier)
{
    for (size_t i = 0; i < seen->count; i++)
        if (!strcmp(seen->items[i], identifier))
            return true;
    return false;
}

static int seen_add(Seen *seen, const char *identifier)
{
    if (seen->count == seen->capacity) {
        size_t next = seen->capacity ? seen->capacity * 2 : 64;
        char **grown = realloc(seen->items, next * sizeof(*grown));
        if (!grown)
            return -1;
        seen->items = grown;
        seen->capacity = next;
    }
    seen->items[seen->count] = strdup(identifier);
    if (!seen->items[seen->count])
        return -1;
    seen->count++;
    return 0;
}

static void seen_free(Seen *seen)
{
    for (size_t i = 0; i < seen->count; i++)
        free(seen->items[i]);
    free(seen->items);
}

static void emit_record(CrawlerContext *context, const char *key, const char *source_url,
                        json_object *record)
{
    char error[SP_ERROR] = {0};
    store_emit(context->store, context->run_id, key, source_url, record, error, sizeof(error));
    json_object_put(record);
}

static void emit_error(CrawlerContext *context, const char *key, const char *message)
{
    json_object *record = json_object_new_object();
    json_object_object_add(record, "type", json_object_new_string("error"));
    json_object_object_add(record, "message", json_object_new_string(message));
    emit_record(context, key, "", record);
}

static void emit_summary(CrawlerContext *context, const char *type, size_t items, size_t files,
                         size_t included, size_t rejected, size_t downloaded, size_t failed)
{
    json_object *record = json_object_new_object();
    json_object_object_add(record, "type", json_object_new_string("summary"));
    json_object_object_add(record, "mode", json_object_new_string(type));
    json_object_object_add(record, "items", json_object_new_int64((int64_t)items));
    json_object_object_add(record, "files", json_object_new_int64((int64_t)files));
    json_object_object_add(record, "included", json_object_new_int64((int64_t)included));
    json_object_object_add(record, "rejected", json_object_new_int64((int64_t)rejected));
    json_object_object_add(record, "downloaded", json_object_new_int64((int64_t)downloaded));
    json_object_object_add(record, "failed", json_object_new_int64((int64_t)failed));
    emit_record(context, "summary", "", record);
}

static int64_t number_field(json_object *object, const char *key)
{
    json_object *value = jg(object, key);
    if (!value)
        return 0;
    if (json_object_is_type(value, json_type_int))
        return json_object_get_int64(value);
    if (json_object_is_type(value, json_type_double))
        return (int64_t)json_object_get_double(value);
    if (json_object_is_type(value, json_type_string))
        return strtoll(json_object_get_string(value), NULL, 10);
    return 0;
}

static void safe_component(const char *input, char *out, size_t cap)
{
    size_t used = 0;
    for (const unsigned char *p = (const unsigned char *)(input ? input : ""); *p && used + 1 < cap;
         p++) {
        unsigned char c = *p;
        if (c == '/' || c == '\\' || c < 0x20 || c == 0x7f)
            c = '_';
        out[used++] = (char)c;
    }
    out[used] = 0;
    while (used && (out[used - 1] == ' ' || out[used - 1] == '.'))
        out[--used] = 0;
    for (size_t i = 0; out[i]; i++)
        if (out[i] != ' ') {
            if (i)
                memmove(out, out + i, strlen(out + i) + 1);
            return;
        }
    snprintf(out, cap, "unknown");
}

static void file_extension(const char *file_name, char *out, size_t cap)
{
    const char *base = strrchr(file_name, '/');
    base = base ? base + 1 : file_name;
    const char *dot = strrchr(base, '.');
    if (dot && dot != base)
        snprintf(out, cap, "%s", dot);
    else
        snprintf(out, cap, ".mp3");
}

static void relative_path(json_object *row, char *out, size_t cap)
{
    char artist[256], file[1024];
    safe_component(js(row, "artist", ""), artist, sizeof(artist));
    const char *file_name = js(row, "file_name", "");
    const char *base = strrchr(file_name, '/');
    base = base ? base + 1 : file_name;
    safe_component(base, file, sizeof(file));
    if (!strcmp(artist, "unknown"))
        snprintf(artist, sizeof(artist), "未知歌手");
    if (!strcmp(file, "unknown")) {
        char title[512], extension[32];
        safe_component(js(row, "title", ""), title, sizeof(title));
        file_extension(file_name, extension, sizeof(extension));
        if (!strcmp(title, "unknown"))
            snprintf(title, sizeof(title), "未命名");
        snprintf(file, sizeof(file), "%s%s", title, extension);
    }
    snprintf(out, cap, "%s/%s", artist, file);
}

static bool relative_from_target(const char *root, const char *target, char *out, size_t cap)
{
    size_t len = strlen(root);
    while (len > 1 && root[len - 1] == '/')
        len--;
    if (!target || strncmp(target, root, len) || target[len] != '/')
        return false;
    snprintf(out, cap, "%s", target + len + 1);
    return true;
}

static int bitrate_of(const char *format)
{
    if (!format || !*format)
        return 0;
    const char *p = format;
    while (*p && (*p < '0' || *p > '9'))
        p++;
    if (*p == '0')
        return (int)strtol(p, NULL, 10);
    return 0;
}

static json_object *source_json(json_object *config, json_object *item, const char *identifier,
                                size_t audio_count)
{
    const char *site = js(config, "site_base_url", "https://archive.org");
    json_object *source = json_object_new_object();
    json_object_object_add(source, "spider_code",
                           json_object_new_string(js(config, "spider_code",
                                                     ARCHIVE_DEFAULT_CODE)));
    json_object_object_add(source, "source", json_object_new_string("archive.org"));
    json_object_object_add(source, "media_type", json_object_new_string("music"));
    json_object_object_add(source, "identifier", json_object_new_string(identifier));
    json_object_object_add(source, "site_base_url", json_object_new_string(site));
    char *details = join_url(site, "details/");
    char *item_url = details ? join_url(details, identifier) : NULL;
    free(details);
    if (item_url) {
        json_object_object_add(source, "item_url", json_object_new_string(item_url));
        free(item_url);
    }
    json_object_object_add(source, "item_type", json_object_new_string("audio"));
    copy_field(source, item, "title");
    copy_field(source, item, "creator");
    copy_field(source, item, "subject");
    copy_field(source, item, "collection");
    copy_field(source, item, "language");
    copy_field(source, item, "description");
    copy_field(source, item, "server");
    copy_field(source, item, "dir");
    json_object_object_add(source, "file_count",
                           json_object_new_int64((int64_t)audio_count));
    char *cover = join_url(site, "services/img/");
    char *cover_url = cover ? join_url(cover, identifier) : NULL;
    free(cover);
    if (cover_url) {
        json_object_object_add(source, "cover_url", json_object_new_string(cover_url));
        free(cover_url);
    }
    json_object *extra = json_object_new_object();
    copy_field(extra, item, "uploader");
    copy_field(extra, item, "date");
    copy_field(extra, item, "addeddate");
    json_object_object_add(source, "metadata", extra);
    return source;
}

static json_object *item_json(json_object *config, json_object *item, json_object *file,
                              const char *identifier, const char *download_base,
                              const char *download_root, ArchiveDecision *decision)
{
    const char *file_name = js(file, "name", "");
    char *encoded = encode_path(file_name);
    char *item_path = join_url(download_base, identifier);
    char *download_url = item_path && encoded ? join_url(item_path, encoded) : NULL;
    free(item_path);
    free(encoded);
    char item_url[SP_PATH];
    snprintf(item_url, sizeof(item_url), "%s/details/%s",
             js(config, "site_base_url", "https://archive.org"), identifier);
    char *direct = NULL;
    const char *server = js(item, "server", "");
    const char *dir = js(item, "dir", "");
    if (*server && *dir) {
        char *server_url = strstr(server, "://") ? strdup(server)
                                                  : join_url("https://", server);
        char *encoded_name = encode_path(file_name);
        char *base = server_url && encoded_name ? join_url(server_url, dir[0] == '/' ? dir + 1 : dir)
                                                : NULL;
        direct = base && encoded_name ? join_url(base, encoded_name) : NULL;
        free(base);
        free(encoded_name);
        free(server_url);
    }

    char item_creator[512] = {0};
    json_object *creator = jg(item, "creator");
    if (creator && json_object_is_type(creator, json_type_string))
        snprintf(item_creator, sizeof(item_creator), "%s", json_object_get_string(creator));
    else if (creator && json_object_is_type(creator, json_type_array) &&
             json_object_array_length(creator)) {
        json_object *first = json_object_array_get_idx(creator, 0);
        if (json_object_is_type(first, json_type_string))
            snprintf(item_creator, sizeof(item_creator), "%s", json_object_get_string(first));
    }
    char artist[256], title[512];
    archive_artist_title(file_name, js(file, "artist", ""), js(file, "title", ""), item_creator,
                         artist, sizeof(artist), title, sizeof(title));
    char relative[SP_PATH];
    safe_component(artist, relative, sizeof(relative));
    if (!strcmp(relative, "unknown"))
        snprintf(relative, sizeof(relative), "未知歌手");
    const char *base = strrchr(file_name, '/');
    base = base ? base + 1 : file_name;
    char safe_file[1024];
    safe_component(base, safe_file, sizeof(safe_file));
    if (!strcmp(safe_file, "unknown")) {
        char safe_title[512], extension[32];
        safe_component(title, safe_title, sizeof(safe_title));
        if (!strcmp(safe_title, "unknown"))
            snprintf(safe_title, sizeof(safe_title), "未命名");
        file_extension(file_name, extension, sizeof(extension));
        snprintf(safe_file, sizeof(safe_file), "%s%s", safe_title, extension);
    }
    size_t used = strlen(relative);
    snprintf(relative + used, sizeof(relative) - used, "/%s", safe_file);
    char target[SP_PATH];
    size_t root_len = strlen(download_root);
    snprintf(target, sizeof(target), "%s%s%s", download_root,
             root_len && download_root[root_len - 1] != '/' ? "/" : "", relative);

    json_object *row = json_object_new_object();
    json_object_object_add(row, "logical_key", json_object_new_string(js(file, "__key", "")));
    json_object_object_add(row, "original_name",
                           json_object_new_string(js(file, "original", "")));
    json_object_object_add(row, "file_name", json_object_new_string(file_name));
    json_object_object_add(row, "download_url",
                           json_object_new_string(download_url ? download_url : ""));
    if (direct)
        json_object_object_add(row, "direct_url", json_object_new_string(direct));
    json_object_object_add(row, "page_url", json_object_new_string(item_url));
    json_object_object_add(row, "download_kind", json_object_new_string("direct"));
    json_object_object_add(row, "file_source", json_object_new_string(js(file, "source", "")));
    json_object_object_add(row, "format", json_object_new_string(js(file, "format", "")));
    json_object_object_add(row, "format_rank",
                           json_object_new_int(archive_format_rank(
                               js(file, "format", ""), jg(config, "format_prefer"))));
    json_object_object_add(row, "bytes",
                           json_object_new_int64((int64_t)archive_file_size(file)));
    json_object_object_add(row, "seconds",
                           json_object_new_double(archive_file_seconds(file)));
    json_object_object_add(row, "content_type", json_object_new_string(js(file, "mime", "")));
    json_object_object_add(row, "remote_sha1", json_object_new_string(js(file, "sha1", "")));
    json_object_object_add(row, "remote_md5", json_object_new_string(js(file, "md5", "")));
    json_object_object_add(row, "remote_crc32", json_object_new_string(js(file, "crc32", "")));
    json_object_object_add(row, "content_key",
                           json_object_new_string(js(file, "sha1", "")));
    json_object_object_add(row, "role", json_object_new_string("track"));
    json_object_object_add(row, "sequence",
                           json_object_new_double((double)number_field(file, "track")));
    json_object_object_add(row, "part_index", json_object_new_int(0));
    json_object_object_add(row, "part_count", json_object_new_int(1));
    json_object_object_add(row, "score", json_object_new_int(decision->score));
    json_object_object_add(row, "decision",
                           json_object_new_string(decision->include ? "include" : "reject"));
    json_object_object_add(row, "reasons", decision->reasons);
    json_object_object_add(row, "target_path", json_object_new_string(target));
    json_object_object_add(row, "artist", json_object_new_string(artist));
    json_object_object_add(row, "title", json_object_new_string(title));
    json_object_object_add(row, "album", json_object_new_string(js(file, "album", "")));
    int64_t track_no = number_field(file, "track");
    if (track_no > 0)
        json_object_object_add(row, "track_no", json_object_new_int64(track_no));
    json_object_object_add(row, "bitrate",
                           json_object_new_int(bitrate_of(js(file, "format", ""))));
    json_object *extra = json_object_new_object();
    copy_field(extra, item, "title");
    copy_field(extra, item, "creator");
    json_object_object_add(row, "extra", extra);
    free(direct);
    free(download_url);
    return row;
}

static int scan_item(CrawlerContext *context, json_object *config, json_object *metadata,
                     const char *identifier, char *error, size_t cap, size_t *files,
                     size_t *included, size_t *rejected)
{
    json_object *item = jg(metadata, "metadata");
    json_object *all_files = jg(metadata, "files");
    if (!item || !json_object_is_type(item, json_type_object) ||
        !all_files || !json_object_is_type(all_files, json_type_array))
        return fail(error, cap, "Metadata for %s has no item or files", identifier);
    size_t audio_count = 0;
    for (size_t i = 0; i < json_object_array_length(all_files); i++) {
        json_object *file = json_object_array_get_idx(all_files, i);
        if (archive_is_audio(js(file, "format", ""), js(file, "name", "")))
            audio_count++;
    }
    json_object *selected = NULL;
    if (archive_select_files(metadata, config, &selected, error, cap))
        return -1;
    size_t logical_count = json_object_array_length(selected);
    json_object *source = source_json(config, item, identifier, audio_count);
    json_object *items = json_object_new_array();
    size_t local_included = 0, local_rejected = 0;
    for (size_t i = 0; i < logical_count; i++) {
        json_object *file = json_object_array_get_idx(selected, i);
        ArchiveDecision decision = {0};
        if (archive_judge(metadata, file, config, logical_count, &decision, error, cap)) {
            json_object_put(selected);
            json_object_put(source);
            json_object_put(items);
            return -1;
        }
        json_object *row = item_json(config, item, file, identifier,
                                     js(config, "download_base",
                                        "https://archive.org/download/"),
                                     js(config, "download_root", ""), &decision);
        json_object_array_add(items, row);
        if (decision.include)
            local_included++;
        else
            local_rejected++;
    }
    int rc = store_download_scan(context->store, context->run_id, source, items, error, cap);
    if (!rc) {
        *files += logical_count;
        *included += local_included;
        *rejected += local_rejected;
        json_object *record = json_object_new_object();
        json_object_object_add(record, "type", json_object_new_string("item"));
        json_object_object_add(record, "identifier", json_object_new_string(identifier));
        json_object_object_add(record, "audio_files",
                               json_object_new_int64((int64_t)audio_count));
        json_object_object_add(record, "selected",
                               json_object_new_int64((int64_t)logical_count));
        json_object_object_add(record, "included", json_object_new_int64((int64_t)local_included));
        json_object_object_add(record, "rejected", json_object_new_int64((int64_t)local_rejected));
        char item_url[SP_PATH];
        snprintf(item_url, sizeof(item_url), "%s/details/%s",
                 js(config, "site_base_url", "https://archive.org"), identifier);
        emit_record(context, identifier, item_url, record);
    }
    json_object_put(selected);
    json_object_put(source);
    json_object_put(items);
    return rc;
}

static int run_scan(CrawlerContext *context, json_object *config, char *error, size_t cap)
{
    json_object *queries = jg(config, "queries");
    if (!queries || !json_object_is_type(queries, json_type_array) ||
        !json_object_array_length(queries))
        return fail(error, cap, "queries must be a non-empty array");
    const char *search_url = js(config, "search_url", "");
    const char *metadata_url = js(config, "metadata_url", "");
    int page_size = ji(config, "page_size", 500);
    int max_pages = ji(config, "max_pages", 50);
    int max_items = ji(config, "max_items", 0);
    int delay = ji(config, "delay_ms", 700);
    Seen seen = {0};
    size_t items_seen = 0, files = 0, included = 0, rejected = 0;
    int result = 0;
    bool partial = false;
    for (size_t q = 0; q < json_object_array_length(queries) && !result; q++) {
        json_object *query = json_object_array_get_idx(queries, q);
        if (!json_object_is_type(query, json_type_string))
            continue;
        for (int page = 1; page <= max_pages; page++) {
            if (crawler_cancelled(context))
                goto done;
            if (max_items > 0 && items_seen >= (size_t)max_items)
                goto done;
            char *url = build_search_url(search_url, json_object_get_string(query), page_size, page);
            if (!url) {
                result = fail(error, cap, "Cannot build search URL");
                goto done;
            }
            json_object *root = fetch_json(context, config, url, error, cap);
            free(url);
            if (!root) {
                emit_error(context, "search", error);
                partial = true;
                break;
            }
            json_object *response = jg(root, "response");
            json_object *docs = jg(response, "docs");
            int num_found = ji(response, "numFound", 0);
            if (!docs || !json_object_is_type(docs, json_type_array) ||
                !json_object_array_length(docs)) {
                json_object_put(root);
                break;
            }
            for (size_t i = 0; i < json_object_array_length(docs); i++) {
                if (crawler_cancelled(context)) {
                    json_object_put(root);
                    goto done;
                }
                if (max_items > 0 && items_seen >= (size_t)max_items) {
                    json_object_put(root);
                    goto done;
                }
                json_object *doc = json_object_array_get_idx(docs, i);
                const char *identifier = js(doc, "identifier", "");
                if (!*identifier || seen_has(&seen, identifier))
                    continue;
                if (seen_add(&seen, identifier)) {
                    json_object_put(root);
                    result = fail(error, cap, "Cannot track identifiers");
                    goto done;
                }
                items_seen++;
                char *encoded_id = encode(identifier);
                char *metadata_link = encoded_id ? join_url(metadata_url, encoded_id) : NULL;
                free(encoded_id);
                if (!metadata_link) {
                    result = fail(error, cap, "Cannot build metadata URL");
                    json_object_put(root);
                    goto done;
                }
                json_object *metadata = fetch_json(context, config, metadata_link, error, cap);
                if (metadata) {
                    if (scan_item(context, config, metadata, identifier, error, cap, &files,
                                  &included, &rejected)) {
                        partial = true;
                        emit_error(context, identifier, error);
                    }
                    json_object_put(metadata);
                } else {
                    partial = true;
                    emit_error(context, identifier, error);
                }
                free(metadata_link);
                if (delay > 0)
                    sleep_ms(delay);
            }
            json_object_put(root);
            if (num_found > 0 && page * page_size >= num_found)
                break;
            if (delay > 0)
                sleep_ms(delay);
        }
    }
done:
    if (crawler_cancelled(context))
        partial = true;
    if (!result && partial)
        result = 1;
    emit_summary(context, "scan", items_seen, files, included, rejected, 0, 0);
    seen_free(&seen);
    return result;
}

static int run_download(CrawlerContext *context, json_object *config, char *error, size_t cap)
{
    const char *spider = js(config, "spider_code", ARCHIVE_DEFAULT_CODE);
    const char *root = js(config, "download_root", "");
    int limit = ji(config, "download_limit", 100);
    int delay = ji(config, "delay_ms", 700);
    json_object *queue = store_download_queue(context->store, spider, "music", limit, error, cap);
    if (!queue)
        return -1;
    size_t downloaded = 0, failed = 0, files = 0;
    for (size_t i = 0; i < json_object_array_length(queue); i++) {
        if (crawler_cancelled(context))
            break;
        json_object *row = json_object_array_get_idx(queue, i);
        const char *url = js(row, "download_url", "");
        if (!*url)
            continue;
        if (store_download_start(context->store, url, context->run_id, error, cap)) {
            emit_error(context, url, error);
            continue;
        }
        char relative[SP_PATH];
        if (!relative_from_target(root, js(row, "target_path", ""), relative, sizeof(relative)))
            relative_path(row, relative, sizeof(relative));
        json_object *options = json_object_new_object();
        json_object_object_add(options, "download_root", json_object_new_string(root));
        json_object_object_add(options, "path", json_object_new_string(relative));
        json_object_object_add(options, "on_exists", json_object_new_string("skip"));
        DownloadFile file = {0};
        char file_error[SP_ERROR] = {0};
        int rc = crawler_save_file(context, options, url, &file, file_error, sizeof(file_error));
        json_object_put(options);
        files++;
        if (!rc) {
            store_download_finish(context->store, url, "downloaded", file.path, file.sha256,
                                  file.bytes, "", error, cap);
            downloaded++;
            json_object *record = json_object_new_object();
            json_object_object_add(record, "type", json_object_new_string("downloaded"));
            json_object_object_add(record, "url", json_object_new_string(url));
            json_object_object_add(record, "path", json_object_new_string(file.path));
            json_object_object_add(record, "bytes", json_object_new_int64(file.bytes));
            emit_record(context, url, url, record);
        } else {
            store_download_finish(context->store, url, "failed", NULL, NULL, -1, file_error,
                                  error, cap);
            failed++;
            json_object *record = json_object_new_object();
            json_object_object_add(record, "type", json_object_new_string("failed"));
            json_object_object_add(record, "url", json_object_new_string(url));
            json_object_object_add(record, "error", json_object_new_string(file_error));
            emit_record(context, url, url, record);
        }
        if (delay > 0)
            sleep_ms(delay);
    }
    json_object_put(queue);
    emit_summary(context, "download", 0, files, 0, 0, downloaded, failed);
    return failed ? 1 : 0;
}

int archive_org_validate(json_object *config, char *error, size_t cap)
{
    if (!config || !json_object_is_type(config, json_type_object))
        return fail(error, cap, "Invalid archive.org configuration");
    const char *mode = js(config, "mode", "scan");
    if (strcmp(mode, "scan") && strcmp(mode, "download"))
        return fail(error, cap, "mode must be scan or download");
    const char *root = js(config, "download_root", "");
    if (root[0] != '/')
        return fail(error, cap, "download_root must be an absolute path");
    if (!*js(config, "spider_code", ""))
        return fail(error, cap, "spider_code is required");
    if (!strcmp(mode, "scan")) {
        json_object *queries = jg(config, "queries");
        if (!queries || !json_object_is_type(queries, json_type_array) ||
            !json_object_array_length(queries))
            return fail(error, cap, "queries must be a non-empty array");
        if (!*js(config, "search_url", "") || !*js(config, "metadata_url", "") ||
            !*js(config, "download_base", ""))
            return fail(error, cap, "search_url, metadata_url and download_base are required");
    }
    int page_size = ji(config, "page_size", 500);
    int max_pages = ji(config, "max_pages", 50);
    int max_items = ji(config, "max_items", 0);
    int delay = ji(config, "delay_ms", 700);
    if (page_size < 1 || page_size > 1000)
        return fail(error, cap, "page_size must be between 1 and 1000");
    if (max_pages < 1)
        return fail(error, cap, "max_pages must be positive");
    if (max_items < 0 || delay < 0)
        return fail(error, cap, "max_items and delay_ms must not be negative");
    if (ji(config, "max_bytes", 33554432) < 1024)
        return fail(error, cap, "max_bytes is too small");
    return 0;
}

int archive_org_run(CrawlerContext *context, char *error, size_t cap)
{
    if (archive_org_validate(context->config, error, cap))
        return -1;
    if (!strcmp(js(context->config, "mode", "scan"), "download"))
        return run_download(context, context->config, error, cap);
    return run_scan(context, context->config, error, cap);
}

int archive_org_register(CrawlerRegistry *registry, char *error, size_t cap)
{
    CrawlerDefinition definition = {.id = "archive-org",
                                    .name = "Archive.org audio",
                                    .validate = archive_org_validate,
                                    .run = archive_org_run};
    if (crawler_register(registry, &definition, error, cap))
        return -1;
    json_object *config = json_tokener_parse(ARCHIVE_DEFAULT_CONFIG);
    if (!config)
        return fail(error, cap, "Invalid archive.org default configuration");
    int rc = crawler_add(registry, ARCHIVE_DEFAULT_CODE, "archive-org", config, error, cap);
    json_object_put(config);
    return rc;
}
