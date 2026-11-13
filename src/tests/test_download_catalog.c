/* Generic download catalog: shared source/item rows, per-type detail rows,
 * queue ordering, download lifecycle, rescan preservation and v4 migration. */
#include "storage/private/storage_internal.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static json_object *parse(const char *raw)
{
    json_object *o = json_tokener_parse(raw);
    assert(o);
    return o;
}

static bool valid_utf8(const char *text)
{
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        size_t length;
        uint32_t cp;
        if (*p < 0x80) {
            p++;
            continue;
        } else if (*p >= 0xC2 && *p <= 0xDF) {
            length = 2;
            cp = *p & 0x1F;
        } else if (*p >= 0xE0 && *p <= 0xEF) {
            length = 3;
            cp = *p & 0x0F;
        } else if (*p >= 0xF0 && *p <= 0xF4) {
            length = 4;
            cp = *p & 0x07;
        } else {
            return false;
        }
        for (size_t i = 1; i < length; i++) {
            if ((p[i] & 0xC0) != 0x80)
                return false;
            cp = (cp << 6) | (p[i] & 0x3F);
        }
        if ((length == 3 && cp < 0x800) || (length == 4 && (cp < 0x10000 || cp > 0x10FFFF)) ||
            (cp >= 0xD800 && cp <= 0xDFFF))
            return false;
        p += length;
    }
    return true;
}

static char *make_path(char *path, size_t cap)
{
    snprintf(path, cap, "/tmp/spider-dlcat-XXXXXX");
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    return path;
}

static Store *open_store(char *path, size_t cap)
{
    char error[SP_ERROR] = {0};
    make_path(path, cap);
    Store *s = store_create(path, error, sizeof(error));
    assert(s);
    return s;
}

static int64_t scalar(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *q = NULL;
    assert(sqlite3_prepare_v2(db, sql, -1, &q, NULL) == SQLITE_OK);
    assert(sqlite3_step(q) == SQLITE_ROW);
    int64_t value = sqlite3_column_int64(q, 0);
    sqlite3_finalize(q);
    return value;
}

static int64_t scalar_text(sqlite3 *db, const char *sql, const char *value)
{
    sqlite3_stmt *q = NULL;
    assert(sqlite3_prepare_v2(db, sql, -1, &q, NULL) == SQLITE_OK);
    sqlite3_bind_text(q, 1, value, -1, SQLITE_TRANSIENT);
    assert(sqlite3_step(q) == SQLITE_ROW);
    int64_t count = sqlite3_column_int64(q, 0);
    sqlite3_finalize(q);
    return count;
}

static char *text_value(sqlite3 *db, const char *sql, const char *first, const char *second)
{
    sqlite3_stmt *q = NULL;
    assert(sqlite3_prepare_v2(db, sql, -1, &q, NULL) == SQLITE_OK);
    sqlite3_bind_text(q, 1, first, -1, SQLITE_TRANSIENT);
    if (second)
        sqlite3_bind_text(q, 2, second, -1, SQLITE_TRANSIENT);
    assert(sqlite3_step(q) == SQLITE_ROW);
    const char *raw = (const char *)sqlite3_column_text(q, 0);
    char *value = strdup(raw ? raw : "");
    assert(value);
    sqlite3_finalize(q);
    return value;
}

static void assert_text(sqlite3 *db, const char *sql, const char *first, const char *second,
                        const char *expected)
{
    char *value = text_value(db, sql, first, second);
    assert(!strcmp(value, expected));
    free(value);
}

static bool object_exists(sqlite3 *db, const char *name)
{
    return scalar_text(db,
                       "SELECT count(*) FROM sqlite_master WHERE name=? AND type IN('table','view')",
                       name) == 1;
}

static json_object *source_json(const char *source, const char *media, const char *identifier,
                                const char *title)
{
    json_object *o = json_object_new_object();
    json_object_object_add(o, "spider_code", json_object_new_string("sp"));
    json_object_object_add(o, "source", json_object_new_string(source));
    json_object_object_add(o, "media_type", json_object_new_string(media));
    json_object_object_add(o, "identifier", json_object_new_string(identifier));
    json_object_object_add(o, "title", json_object_new_string(title));
    json_object_object_add(o, "creator", json_object_new_string("creator"));
    json_object_object_add(o, "subject", parse("[\"Chinese Music\"]"));
    json_object_object_add(o, "collection", parse("[\"opensource_audio\"]"));
    json_object_object_add(o, "tags", parse("[\"mandopop\"]"));
    json_object_object_add(o, "language", json_object_new_string("chi"));
    json_object_object_add(o, "site_base_url", json_object_new_string("https://archive.org"));
    json_object_object_add(o, "item_url",
                           json_object_new_string("https://archive.org/details/item"));
    json_object_object_add(o, "item_type", json_object_new_string("album"));
    json_object_object_add(o, "server", json_object_new_string("ia.example"));
    json_object_object_add(o, "dir", json_object_new_string("/0/items/item"));
    json_object_object_add(o, "file_count", json_object_new_int(2));
    json_object_object_add(o, "description", json_object_new_string("description"));
    json_object_object_add(o, "metadata", parse("{\"site\":\"raw\"}"));
    return o;
}

static json_object *music_item(const char *key, const char *url, const char *artist,
                               const char *title, int score, const char *decision)
{
    json_object *o = json_object_new_object();
    json_object_object_add(o, "logical_key", json_object_new_string(key));
    json_object_object_add(o, "file_name", json_object_new_string("track.mp3"));
    json_object_object_add(o, "download_url", json_object_new_string(url));
    json_object_object_add(o, "format", json_object_new_string("VBR MP3"));
    json_object_object_add(o, "format_rank", json_object_new_int(1));
    json_object_object_add(o, "bytes", json_object_new_int(1234));
    json_object_object_add(o, "remote_sha1", json_object_new_string(key));
    json_object_object_add(o, "score", json_object_new_int(score));
    json_object_object_add(o, "decision", json_object_new_string(decision));
    json_object_object_add(o, "artist", json_object_new_string(artist));
    json_object_object_add(o, "title", json_object_new_string(title));
    json_object_object_add(o, "album", json_object_new_string("album"));
    json_object_object_add(o, "track_no", json_object_new_int(1));
    json_object_object_add(o, "seconds", json_object_new_double(180.5));
    json_object_object_add(o, "bitrate", json_object_new_int(128));
    json_object_object_add(o, "options",
                           parse("{\"headers\":{\"Referer\":\"https://archive.org\"}}"));
    json_object_object_add(o, "extra", parse("{\"site_field\":\"value\"}"));
    return o;
}

static void seed_music(Store *s)
{
    char error[SP_ERROR] = {0};
    json_object *source = source_json("archive.org", "music", "vol-1", "Volume 1");
    json_object *items = json_object_new_array();
    json_object_array_add(items, music_item("a", "https://archive.org/download/vol-1/a.mp3",
                                            "Artist A", "Title A", 5, "include"));
    json_object_array_add(items, music_item("b", "https://archive.org/download/vol-1/b.mp3",
                                            "Artist B", "Title B", 3, "include"));
    assert(store_download_scan(s, 7, source, items, error, sizeof(error)) == 0);
    json_object_put(source);
    json_object_put(items);
}

static json_object *fetch_queue(Store *s, const char *media)
{
    char error[SP_ERROR] = {0};
    json_object *queue = store_download_queue(s, "sp", media, 100, 0, 1, error, sizeof(error));
    assert(queue && json_object_is_type(queue, json_type_array));
    return queue;
}

static void test_schema(void)
{
    char path[64];
    Store *s = open_store(path, sizeof(path));
    assert(scalar(s->db, "PRAGMA user_version") == 4);
    assert(object_exists(s->db, "download_sources"));
    assert(object_exists(s->db, "download_items"));
    assert(object_exists(s->db, "music_tracks"));
    assert(object_exists(s->db, "novel_files"));
    assert(object_exists(s->db, "music_downloads"));
    assert(object_exists(s->db, "novel_downloads"));
    store_destroy(s);
    unlink(path);
}

static void test_music_scan_and_queue(void)
{
    char path[64];
    Store *s = open_store(path, sizeof(path));
    seed_music(s);
    json_object *queue = fetch_queue(s, "music");
    assert(json_object_array_length(queue) == 2);
    json_object *top = json_object_array_get_idx(queue, 0);
    assert(!strcmp(js(top, "artist", ""), "Artist A"));
    assert(!strcmp(js(top, "title", ""), "Title A"));
    assert(!strcmp(js(top, "download_url", ""), "https://archive.org/download/vol-1/a.mp3"));
    assert(!strcmp(js(top, "item_title", ""), "Volume 1"));
    assert(!strcmp(js(top, "item_identifier", ""), "vol-1"));
    assert(ji(top, "score", 0) == 5);
    json_object *options = jg(top, "options_json");
    json_object *extra = jg(top, "extra_json");
    json_object *subject = jg(top, "subject");
    assert(options && json_object_is_type(options, json_type_object));
    assert(extra && json_object_is_type(extra, json_type_object));
    assert(subject && json_object_is_type(subject, json_type_array));
    assert(strstr(json_object_to_json_string(options), "Referer"));
    assert(strstr(json_object_to_json_string(extra), "site_field"));
    assert(strstr(json_object_to_json_string(subject), "Chinese Music"));
    json_object_put(queue);
    store_destroy(s);
    unlink(path);
}

static void test_lifecycle_and_retry(void)
{
    char path[64];
    Store *s = open_store(path, sizeof(path));
    seed_music(s);
    char error[SP_ERROR] = {0};
    const char *url = "https://archive.org/download/vol-1/a.mp3";
    assert(store_download_start(s, url, 8, error, sizeof(error)) == 0);
    assert(scalar_text(s->db, "SELECT attempts FROM download_items WHERE download_url=?1", url) == 1);
    assert(store_download_finish(s, url, "downloaded", "/tmp/a.mp3", "localhash", 1234, "", error,
                                 sizeof(error)) == 0);
    assert_text(s->db, "SELECT status FROM download_items WHERE download_url=?1", url, NULL,
                "downloaded");
    assert_text(s->db, "SELECT target_path FROM download_items WHERE download_url=?1", url, NULL,
                "/tmp/a.mp3");
    json_object *queue = fetch_queue(s, "music");
    assert(json_object_array_length(queue) == 1);
    json_object_put(queue);

    /* Failed rows stay queued indefinitely; attempts never give up. */
    const char *url2 = "https://archive.org/download/vol-1/b.mp3";
    for (int i = 0; i < 5; i++) {
        assert(store_download_start(s, url2, 8, error, sizeof(error)) == 0);
        assert(store_download_finish(s, url2, "failed", NULL, NULL, -1, "boom", error,
                                     sizeof(error)) == 0);
    }
    assert(scalar_text(s->db, "SELECT attempts FROM download_items WHERE download_url=?1", url2) ==
           5);
    queue = fetch_queue(s, "music");
    assert(json_object_array_length(queue) == 1);
    json_object_put(queue);

    json_object *summary = store_download_summary(s, "sp", error, sizeof(error));
    assert(summary);
    bool downloaded = false, failed = false;
    for (size_t i = 0; i < json_object_array_length(summary); i++) {
        json_object *row = json_object_array_get_idx(summary, i);
        if (!strcmp(js(row, "status", ""), "downloaded"))
            downloaded = ji(row, "count", 0) == 1;
        if (!strcmp(js(row, "status", ""), "failed"))
            failed = ji(row, "count", 0) == 1;
    }
    assert(downloaded && failed);
    json_object_put(summary);
    store_destroy(s);
    unlink(path);
}

static void test_rescan_preserves_state(void)
{
    char path[64];
    Store *s = open_store(path, sizeof(path));
    seed_music(s);
    char error[SP_ERROR] = {0};
    const char *url = "https://archive.org/download/vol-1/a.mp3";
    assert(store_download_start(s, url, 8, error, sizeof(error)) == 0);
    assert(store_download_finish(s, url, "downloaded", "/tmp/a.mp3", "hash", 10, "", error,
                                 sizeof(error)) == 0);

    /* A rescan that finds a FLAC keeps the downloaded MP3 and its status. */
    json_object *source = source_json("archive.org", "music", "vol-1", "Volume 1 edited");
    json_object *items = json_object_new_array();
    json_object *better = music_item("a", "https://archive.org/download/vol-1/a.flac",
                                     "Artist A", "Title A edited", 9, "include");
    json_object_object_add(better, "file_name", json_object_new_string("a.flac"));
    json_object_object_add(better, "format", json_object_new_string("24bit Flac"));
    json_object_array_add(items, better);
    json_object *rejected = music_item("b", "https://archive.org/download/vol-1/b.mp3",
                                       "Artist B", "Title B", 1, "include");
    json_object_array_add(items, rejected);
    assert(store_download_scan(s, 9, source, items, error, sizeof(error)) == 0);
    json_object_put(source);
    json_object_put(items);

    assert_text(s->db, "SELECT download_url FROM download_items WHERE logical_key='a'", "a", NULL,
                url);
    assert_text(s->db, "SELECT status FROM download_items WHERE logical_key='a'", "a", NULL,
                "downloaded");
    assert(scalar_text(s->db,
                       "SELECT score FROM download_items WHERE logical_key='a'", "a") == 9);
    assert_text(s->db,
                "SELECT title FROM music_tracks WHERE id=(SELECT id FROM download_items WHERE "
                "logical_key='a')",
                "a", NULL, "Title A edited");

    /* Manual decision survives a later rescan. */
    assert(store_download_decide(s, "https://archive.org/download/vol-1/b.mp3", "reject", 1, error,
                                 sizeof(error)) == 0);
    source = source_json("archive.org", "music", "vol-1", "Volume 1");
    items = json_object_new_array();
    json_object_array_add(items, music_item("b", "https://archive.org/download/vol-1/b.mp3",
                                            "Artist B", "Title B", 8, "include"));
    assert(store_download_scan(s, 10, source, items, error, sizeof(error)) == 0);
    json_object_put(source);
    json_object_put(items);
    assert_text(s->db, "SELECT decision FROM download_items WHERE logical_key='b'", "b", NULL,
                "reject");
    assert(scalar_text(s->db, "SELECT curated FROM download_items WHERE logical_key='b'", "b") == 1);
    store_destroy(s);
    unlink(path);
}

static void test_novel_and_multiple_sites(void)
{
    char path[64];
    Store *s = open_store(path, sizeof(path));
    char error[SP_ERROR] = {0};
    json_object *source = source_json("novel.example", "novel", "book-1", "Book 1");
    json_object *items = json_object_new_array();
    json_object *chapter = json_object_new_object();
    json_object_object_add(chapter, "logical_key", json_object_new_string("ch-001"));
    json_object_object_add(chapter, "file_name", json_object_new_string("chapter-001.html"));
    json_object_object_add(chapter, "download_url",
                           json_object_new_string("https://novel.example/book-1/ch-001.html"));
    json_object_object_add(chapter, "format", json_object_new_string("HTML"));
    json_object_object_add(chapter, "score", json_object_new_int(4));
    json_object_object_add(chapter, "decision", json_object_new_string("include"));
    json_object_object_add(chapter, "author", json_object_new_string("Author"));
    json_object_object_add(chapter, "title", json_object_new_string("Chapter 1"));
    json_object_object_add(chapter, "series", json_object_new_string("Series"));
    json_object_object_add(chapter, "chapter_no", json_object_new_int(1));
    json_object_object_add(chapter, "language", json_object_new_string("zh"));
    json_object_object_add(chapter, "word_count", json_object_new_int(3000));
    json_object_array_add(items, chapter);
    assert(store_download_scan(s, 1, source, items, error, sizeof(error)) == 0);
    json_object_put(source);
    json_object_put(items);

    json_object *novels = fetch_queue(s, "novel");
    assert(json_object_array_length(novels) == 1);
    json_object *row = json_object_array_get_idx(novels, 0);
    assert(!strcmp(js(row, "author", ""), "Author"));
    assert(ji(row, "chapter_no", 0) == 1);
    assert(!strcmp(js(row, "item_title", ""), "Book 1"));
    json_object_put(novels);

    /* A second site contributes to the same shared queue. */
    seed_music(s);
    json_object *music = fetch_queue(s, "music");
    assert(json_object_array_length(music) == 2);
    json_object_put(music);
    store_destroy(s);
    unlink(path);
}

static void test_duplicate_url_rolls_back(void)
{
    char path[64];
    Store *s = open_store(path, sizeof(path));
    char error[SP_ERROR] = {0};
    json_object *source = source_json("archive.org", "music", "vol-1", "Volume 1");
    json_object *items = json_object_new_array();
    json_object_array_add(items, music_item("a", "https://archive.org/download/vol-1/a.mp3",
                                            "Artist A", "Title A", 5, "include"));
    json_object_array_add(items, music_item("b", "https://archive.org/download/vol-1/a.mp3",
                                            "Artist B", "Title B", 3, "include"));
    assert(store_download_scan(s, 1, source, items, error, sizeof(error)) < 0);
    assert(strstr(error, "Duplicate download URL"));
    assert(scalar(s->db, "SELECT count(*) FROM download_sources") == 0);
    assert(scalar(s->db, "SELECT count(*) FROM download_items") == 0);
    json_object_put(source);
    json_object_put(items);
    store_destroy(s);
    unlink(path);
}

static void test_migration_from_v3(void)
{
    char path[64];
    Store *s = open_store(path, sizeof(path));
    store_destroy(s);
    sqlite3 *db = NULL;
    assert(sqlite3_open(path, &db) == SQLITE_OK);
    const char *drop =
        "PRAGMA foreign_keys=OFF;"
        "DROP VIEW IF EXISTS music_downloads;DROP VIEW IF EXISTS novel_downloads;"
        "DROP TABLE IF EXISTS music_tracks;DROP TABLE IF EXISTS novel_files;"
        "DROP TABLE IF EXISTS download_items;DROP TABLE IF EXISTS download_sources;"
        "PRAGMA user_version=3;";
    assert(sqlite3_exec(db, drop, NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_close(db);

    char error[SP_ERROR] = {0};
    s = store_create(path, error, sizeof(error));
    assert(s);
    assert(scalar(s->db, "PRAGMA user_version") == 4);
    assert(object_exists(s->db, "download_sources"));
    assert(object_exists(s->db, "download_items"));
    assert(object_exists(s->db, "music_tracks"));
    assert(object_exists(s->db, "novel_files"));
    assert(object_exists(s->db, "music_downloads"));
    store_destroy(s);
    unlink(path);
}

static void test_library_pages(void)
{
    char path[64];
    Store *s = open_store(path, sizeof(path));
    char error[SP_ERROR] = {0};
    seed_music(s);
    assert(!store_download_sha1_exists(s, "a", "vol-1", "a", error, sizeof(error)));
    assert(store_download_sha1_exists(s, "a", "vol-1", "b", error, sizeof(error)));
    assert(!store_download_sha1_exists(s, "missing", "vol-1", "a", error, sizeof(error)));
    json_object *all = store_download_queue(s, "sp", "music", 0, 0, 1, error, sizeof(error));
    assert(all && json_object_array_length(all) == 2);
    json_object_put(all);
    json_object *part0 = store_download_queue(s, "sp", "music", 100, 0, 2, error,
                                              sizeof(error));    json_object *part1 = store_download_queue(s, "sp", "music", 100, 1, 2, error,
                                              sizeof(error));
    assert(part0 && json_object_array_length(part0) == 1);
    assert(part1 && json_object_array_length(part1) == 1);
    int64_t id0 = ji(json_object_array_get_idx(part0, 0), "id", 0);
    int64_t id1 = ji(json_object_array_get_idx(part1, 0), "id", 0);
    assert(id0 % 2 == 0 && id1 % 2 == 1);
    json_object_put(part0);
    json_object_put(part1);
    json_object *source = source_json("novel.example", "novel", "book-1", "Book 1");
    json_object *items = json_object_new_array();
    json_object *chapter = json_object_new_object();
    json_object_object_add(chapter, "logical_key", json_object_new_string("ch-1"));
    json_object_object_add(chapter, "file_name", json_object_new_string("ch1.html"));
    json_object_object_add(chapter, "download_url",
                           json_object_new_string("https://novel.example/book-1/ch1.html"));
    json_object_object_add(chapter, "format", json_object_new_string("HTML"));
    json_object_object_add(chapter, "score", json_object_new_int(4));
    json_object_object_add(chapter, "decision", json_object_new_string("include"));
    json_object_object_add(chapter, "author", json_object_new_string("Author"));
    json_object_object_add(chapter, "title", json_object_new_string("Chapter 1"));
    json_object_object_add(chapter, "chapter_no", json_object_new_int(1));
    json_object_object_add(chapter, "word_count", json_object_new_int(3000));
    json_object_array_add(items, chapter);
    assert(store_download_scan(s, 1, source, items, error, sizeof(error)) == 0);
    json_object_put(source);
    json_object_put(items);

    json_object *page = store_library_page(s, "music", "", 1, 1, error, sizeof(error));
    assert(page);
    assert(ji(page, "total", 0) == 2);
    assert(ji(page, "pages", 0) == 2);
    assert(ji(page, "page", 0) == 1);
    json_object *rows = jg(page, "items");
    assert(rows && json_object_array_length(rows) == 1);
    assert(!strcmp(js(json_object_array_get_idx(rows, 0), "title", ""), "Title B"));
    json_object_put(page);

    page = store_library_page(s, "music", "", 2, 1, error, sizeof(error));
    assert(page && ji(page, "page", 0) == 2);
    rows = jg(page, "items");
    assert(rows && json_object_array_length(rows) == 1);
    assert(!strcmp(js(json_object_array_get_idx(rows, 0), "title", ""), "Title A"));
    json_object_put(page);

    page = store_library_page(s, "music", "", 3, 1, error, sizeof(error));
    assert(page && ji(page, "page", 0) == 2);
    json_object_put(page);

    page = store_library_page(s, "music", "pending", 1, 20, error, sizeof(error));
    assert(page && ji(page, "total", 0) == 2);
    json_object_put(page);
    page = store_library_page(s, "music", "downloaded", 1, 20, error, sizeof(error));
    assert(page && ji(page, "total", 0) == 0);
    json_object_put(page);

    page = store_library_page(s, "novel", "", 1, 20, error, sizeof(error));
    assert(page && ji(page, "total", 0) == 1);
    rows = jg(page, "items");
    assert(rows && json_object_array_length(rows) == 1);
    assert(!strcmp(js(json_object_array_get_idx(rows, 0), "author", ""), "Author"));
    assert(ji(json_object_array_get_idx(rows, 0), "word_count", 0) == 3000);
    json_object_put(page);

    assert(store_library_page(s, "radio", "", 1, 20, error, sizeof(error)) == NULL);
    assert(store_library_page(s, "music", "", 0, 20, error, sizeof(error)) == NULL);
    assert(store_library_page(s, "music", "", 1, 0, error, sizeof(error)) == NULL);
    assert(store_library_page(s, "music", "bogus", 1, 20, error, sizeof(error)) == NULL);
    store_destroy(s);
    unlink(path);
}

static void test_download_recovery(void)
{
    char path[64];
    Store *s = open_store(path, sizeof(path));
    char error[SP_ERROR] = {0};
    seed_music(s);
    const char *stale = "https://archive.org/download/vol-1/a.mp3";
    assert(store_download_start(s, stale, 999, error, sizeof(error)) == 0);
    assert(store_download_recover(s, "sp", error, sizeof(error)) == 0);
    assert_text(s->db, "SELECT status FROM download_items WHERE download_url=?1", stale, NULL,
                "pending");

    json_object *config = parse("{\"name\":\"recovery\"}");
    int64_t spider = store_save_spider(s, 0, config, error, sizeof(error));
    assert(spider > 0);
    int64_t run = store_start_run(s, spider, error, sizeof(error));
    assert(run > 0);
    json_object *claimed = store_claim_run(s, NULL, 0, error, sizeof(error));
    assert(claimed);
    json_object_put(claimed);
    const char *active = "https://archive.org/download/vol-1/b.mp3";
    assert(store_download_start(s, active, run, error, sizeof(error)) == 0);
    assert(store_download_recover(s, "sp", error, sizeof(error)) == 0);
    assert_text(s->db, "SELECT status FROM download_items WHERE download_url=?1", active, NULL,
                "downloading");
    json_object_put(config);
    store_destroy(s);
    unlink(path);
}

static void test_library_repairs_invalid_utf8(void)
{
    char path[64];
    Store *s = open_store(path, sizeof(path));
    char error[SP_ERROR] = {0};
    json_object *source = source_json("archive.org", "music", "vol-bad", "Bad Volume");
    json_object *items = json_object_new_array();
    json_object_array_add(items, music_item("bad", "https://archive.org/download/vol-bad/bad.mp3",
                                            "Artist \xe3\x80", "Title", 5, "include"));
    assert(store_download_scan(s, 7, source, items, error, sizeof(error)) == 0);
    json_object_put(source);
    json_object_put(items);
    json_object *page = store_library_page(s, "music", "", 1, 20, error, sizeof(error));
    assert(page);
    const char *raw = json_object_to_json_string_ext(page, JSON_C_TO_STRING_PLAIN);
    assert(valid_utf8(raw));
    assert(strstr(raw, "\xef\xbf\xbd"));
    json_object_put(page);
    store_destroy(s);
    unlink(path);
}

int main(void)
{
    test_schema();
    test_music_scan_and_queue();
    test_lifecycle_and_retry();
    test_rescan_preserves_state();
    test_novel_and_multiple_sites();
    test_duplicate_url_rolls_back();
    test_migration_from_v3();
    test_library_pages();
    test_library_repairs_invalid_utf8();
    test_download_recovery();
    puts("download catalog passed");
    return 0;
}
