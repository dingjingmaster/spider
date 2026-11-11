#include "storage/private/storage_internal.h"
#include <stdlib.h>
#include <string.h>

static sqlite3_stmt *prepare(Store *s, const char *sql)
{
    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(s->db, sql, -1, &q, NULL) != SQLITE_OK)
        return NULL;
    return q;
}

static void text(sqlite3_stmt *q, int i, const char *value)
{
    if (value)
        sqlite3_bind_text(q, i, value, -1, SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(q, i);
}

static void json_text(sqlite3_stmt *q, int i, json_object *object, const char *key, size_t cap)
{
    json_object *value = jg(object, key);
    if (!value || json_object_is_type(value, json_type_null)) {
        sqlite3_bind_null(q, i);
        return;
    }
    const char *raw = json_object_is_type(value, json_type_string)
                          ? json_object_get_string(value)
                          : json_object_to_json_string_ext(value, JSON_C_TO_STRING_PLAIN);
    sqlite3_bind_text(q, i, raw, (int)strnlen(raw, cap), SQLITE_TRANSIENT);
}

static void optional_int(sqlite3_stmt *q, int i, json_object *object, const char *key)
{
    json_object *value = jg(object, key);
    if (value && json_object_is_type(value, json_type_int))
        sqlite3_bind_int64(q, i, json_object_get_int64(value));
    else
        sqlite3_bind_null(q, i);
}

static void optional_double(sqlite3_stmt *q, int i, json_object *object, const char *key)
{
    json_object *value = jg(object, key);
    if (value && (json_object_is_type(value, json_type_double) ||
                  json_object_is_type(value, json_type_int)))
        sqlite3_bind_double(q, i, json_object_get_double(value));
    else
        sqlite3_bind_null(q, i);
}

static int64_t int_value(json_object *object, const char *key, int64_t fallback)
{
    json_object *value = jg(object, key);
    return value && json_object_is_type(value, json_type_int) ? json_object_get_int64(value)
                                                              : fallback;
}

static const char *string_value(json_object *object, const char *key, const char *fallback)
{
    json_object *value = jg(object, key);
    return value && json_object_is_type(value, json_type_string) ? json_object_get_string(value)
                                                                 : fallback;
}

static bool json_key(const char *key)
{
    return !strcmp(key, "subject") || !strcmp(key, "collection") || !strcmp(key, "tags") ||
           !strcmp(key, "reasons") || !strcmp(key, "options_json") ||
           !strcmp(key, "extra_json") || !strcmp(key, "metadata_json");
}

static json_object *row(sqlite3_stmt *q)
{
    json_object *object = json_object_new_object();
    for (int i = 0; i < sqlite3_column_count(q); i++) {
        const char *key = sqlite3_column_name(q, i);
        int type = sqlite3_column_type(q, i);
        json_object *value = NULL;
        if (type == SQLITE_INTEGER)
            value = json_object_new_int64(sqlite3_column_int64(q, i));
        else if (type == SQLITE_FLOAT)
            value = json_object_new_double(sqlite3_column_double(q, i));
        else if (type != SQLITE_NULL) {
            const char *raw = (const char *)sqlite3_column_text(q, i);
            if (json_key(key)) {
                value = json_tokener_parse(raw);
                if (!value)
                    value = json_object_new_string(raw);
            } else
                value = json_object_new_string(raw);
        }
        if (value)
            json_object_object_add(object, key, value);
    }
    return object;
}

static int upsert_source(Store *s, json_object *source, int64_t *id, char *e, size_t n)
{
    const char *spider = string_value(source, "spider_code", "");
    const char *site = string_value(source, "source", "");
    const char *media = string_value(source, "media_type", "");
    const char *identifier = string_value(source, "identifier", "");
    if (!*spider || !*site || !*media || !*identifier)
        return fail(e, n, "Download source requires spider_code, source, media_type and identifier");
    int64_t now = now_ms();
    sqlite3_stmt *q = prepare(
        s,
        "INSERT INTO download_sources(spider_code,source,media_type,identifier,site_base_url,"
        "item_url,item_type,title,creator,subject,collection,tags,language,region,license,rights,"
        "cover_url,description,server,dir,file_count,metadata_json,extra_json,added_at,scanned_at,"
        "created_at,updated_at) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,"
        "?17,?18,?19,?20,?21,?22,?23,?24,?25,?26,?27) ON CONFLICT(spider_code,source,media_type,"
        "identifier) DO UPDATE SET site_base_url=excluded.site_base_url,item_url=excluded.item_url,"
        "item_type=excluded.item_type,title=excluded.title,creator=excluded.creator,"
        "subject=excluded.subject,collection=excluded.collection,tags=excluded.tags,"
        "language=excluded.language,region=excluded.region,license=excluded.license,"
        "rights=excluded.rights,cover_url=excluded.cover_url,description=excluded.description,"
        "server=excluded.server,dir=excluded.dir,file_count=excluded.file_count,"
        "metadata_json=excluded.metadata_json,extra_json=excluded.extra_json,"
        "added_at=coalesce(excluded.added_at,download_sources.added_at),"
        "scanned_at=excluded.scanned_at,updated_at=excluded.updated_at RETURNING id");
    if (!q)
        return fail(e, n, "SQLite: %s", sqlite3_errmsg(s->db));
    text(q, 1, spider);
    text(q, 2, site);
    text(q, 3, media);
    text(q, 4, identifier);
    text(q, 5, string_value(source, "site_base_url", NULL));
    text(q, 6, string_value(source, "item_url", NULL));
    text(q, 7, string_value(source, "item_type", NULL));
    text(q, 8, string_value(source, "title", NULL));
    text(q, 9, string_value(source, "creator", NULL));
    json_text(q, 10, source, "subject", 65536);
    json_text(q, 11, source, "collection", 65536);
    json_text(q, 12, source, "tags", 65536);
    text(q, 13, string_value(source, "language", NULL));
    text(q, 14, string_value(source, "region", NULL));
    text(q, 15, string_value(source, "license", NULL));
    text(q, 16, string_value(source, "rights", NULL));
    text(q, 17, string_value(source, "cover_url", NULL));
    json_text(q, 18, source, "description", 8192);
    text(q, 19, string_value(source, "server", NULL));
    text(q, 20, string_value(source, "dir", NULL));
    optional_int(q, 21, source, "file_count");
    json_text(q, 22, source, jg(source, "metadata_json") ? "metadata_json" : "metadata", 65536);
    json_text(q, 23, source, jg(source, "extra_json") ? "extra_json" : "extra", 65536);
    optional_int(q, 24, source, "added_at");
    sqlite3_bind_int64(q, 25, int_value(source, "scanned_at", now));
    sqlite3_bind_int64(q, 26, now);
    sqlite3_bind_int64(q, 27, now);
    int rc = sqlite3_step(q);
    if (rc == SQLITE_ROW)
        *id = sqlite3_column_int64(q, 0);
    sqlite3_finalize(q);
    return rc == SQLITE_ROW ? 0
                            : fail(e, n, "Cannot write download source: %s", sqlite3_errmsg(s->db));
}

static int url_conflict(Store *s, int64_t source_id, const char *logical, const char *url,
                        char *e, size_t n)
{
    sqlite3_stmt *q =
        prepare(s, "SELECT status FROM download_items WHERE source_id=?1 AND logical_key=?2");
    if (!q)
        return fail(e, n, "SQLite: %s", sqlite3_errmsg(s->db));
    sqlite3_bind_int64(q, 1, source_id);
    text(q, 2, logical);
    bool downloaded = false;
    if (sqlite3_step(q) == SQLITE_ROW) {
        const char *status = (const char *)sqlite3_column_text(q, 0);
        downloaded = status && !strcmp(status, "downloaded");
    }
    sqlite3_finalize(q);
    if (downloaded)
        return 0;
    q = prepare(s,
                "SELECT count(*) FROM download_items WHERE download_url=?1 AND NOT(source_id=?2 "
                "AND logical_key=?3)");
    if (!q)
        return fail(e, n, "SQLite: %s", sqlite3_errmsg(s->db));
    text(q, 1, url);
    sqlite3_bind_int64(q, 2, source_id);
    text(q, 3, logical);
    int rc = sqlite3_step(q);
    int64_t count = rc == SQLITE_ROW ? sqlite3_column_int64(q, 0) : -1;
    sqlite3_finalize(q);
    if (count < 0)
        return fail(e, n, "SQLite: %s", sqlite3_errmsg(s->db));
    return count ? fail(e, n, "Duplicate download URL") : 0;
}

static int write_detail(Store *s, int64_t id, const char *media, json_object *item, char *e,
                        size_t n)
{
    sqlite3_stmt *q;
    if (!strcmp(media, "music")) {
        q = prepare(s,
                    "INSERT INTO music_tracks(id,artist,title,album,track_no,seconds,bitrate) "
                    "VALUES(?1,?2,?3,?4,?5,?6,?7) ON CONFLICT(id) DO UPDATE SET "
                    "artist=excluded.artist,title=excluded.title,album=excluded.album,"
                    "track_no=excluded.track_no,seconds=excluded.seconds,bitrate=excluded.bitrate");
        if (!q)
            return fail(e, n, "SQLite: %s", sqlite3_errmsg(s->db));
        sqlite3_bind_int64(q, 1, id);
        text(q, 2, string_value(item, "artist", NULL));
        text(q, 3, string_value(item, "title", NULL));
        text(q, 4, string_value(item, "album", NULL));
        optional_int(q, 5, item, "track_no");
        optional_double(q, 6, item, "seconds");
        optional_int(q, 7, item, "bitrate");
    } else if (!strcmp(media, "novel")) {
        q = prepare(s,
                    "INSERT INTO novel_files(id,author,title,series,chapter_no,language,word_count) "
                    "VALUES(?1,?2,?3,?4,?5,?6,?7) ON CONFLICT(id) DO UPDATE SET "
                    "author=excluded.author,title=excluded.title,series=excluded.series,"
                    "chapter_no=excluded.chapter_no,language=excluded.language,"
                    "word_count=excluded.word_count");
        if (!q)
            return fail(e, n, "SQLite: %s", sqlite3_errmsg(s->db));
        sqlite3_bind_int64(q, 1, id);
        text(q, 2, string_value(item, "author", NULL));
        text(q, 3, string_value(item, "title", NULL));
        text(q, 4, string_value(item, "series", NULL));
        optional_int(q, 5, item, "chapter_no");
        text(q, 6, string_value(item, "language", NULL));
        optional_int(q, 7, item, "word_count");
    } else {
        return 0;
    }
    int rc = sqlite3_step(q);
    sqlite3_finalize(q);
    return rc == SQLITE_DONE ? 0 : fail(e, n, "Cannot write %s detail: %s", media,
                                        sqlite3_errmsg(s->db));
}

static int upsert_item(Store *s, int64_t source_id, int64_t run_id, const char *spider,
                       const char *source_media, json_object *item, char *e, size_t n)
{
    const char *logical = string_value(item, "logical_key", "");
    const char *file_name = string_value(item, "file_name", "");
    const char *url = string_value(item, "download_url", "");
    const char *media = string_value(item, "media_type", source_media);
    if (!*logical || !*file_name || !*url || !*media)
        return fail(e, n, "Download item requires logical_key, file_name, download_url and media_type");
    if (url_conflict(s, source_id, logical, url, e, n))
        return -1;
    int64_t now = now_ms();
    sqlite3_stmt *q = prepare(
        s,
        "INSERT INTO download_items(source_id,spider_code,media_type,logical_key,original_name,"
        "file_name,download_url,direct_url,page_url,download_kind,options_json,auth_key,content_type,"
        "file_source,format,format_rank,bytes,remote_sha1,remote_md5,remote_crc32,expected_sha256,"
        "content_key,role,sequence,part_index,part_count,group_key,score,decision,reasons,curated,"
        "notes,target_path,status,attempts,error,http_status,retry_after,next_attempt_at,"
        "local_sha256,local_bytes,downloaded_at,last_run_id,extra_json,created_at,updated_at) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19,?20,?21,"
        "?22,?23,?24,?25,?26,?27,?28,?29,?30,?31,?32,?33,?34,?35,?36,?37,?38,?39,?40,?41,?42,"
        "?43,?44,?45,?46) ON CONFLICT(source_id,logical_key) DO UPDATE SET "
        "original_name=excluded.original_name,"
        "file_name=CASE WHEN download_items.status='downloaded' THEN download_items.file_name ELSE "
        "excluded.file_name END,"
        "download_url=CASE WHEN download_items.status='downloaded' THEN download_items.download_url "
        "ELSE excluded.download_url END,"
        "direct_url=excluded.direct_url,page_url=excluded.page_url,"
        "download_kind=excluded.download_kind,options_json=excluded.options_json,"
        "auth_key=excluded.auth_key,content_type=excluded.content_type,"
        "file_source=excluded.file_source,"
        "format=CASE WHEN download_items.status='downloaded' THEN download_items.format ELSE "
        "excluded.format END,"
        "format_rank=excluded.format_rank,bytes=excluded.bytes,remote_sha1=excluded.remote_sha1,"
        "remote_md5=excluded.remote_md5,remote_crc32=excluded.remote_crc32,"
        "expected_sha256=excluded.expected_sha256,content_key=excluded.content_key,"
        "role=excluded.role,sequence=excluded.sequence,part_index=excluded.part_index,"
        "part_count=excluded.part_count,group_key=excluded.group_key,score=excluded.score,"
        "decision=CASE WHEN download_items.curated=1 THEN download_items.decision ELSE "
        "excluded.decision END,"
        "reasons=excluded.reasons,curated=download_items.curated,notes=download_items.notes,"
        "target_path=download_items.target_path,status=download_items.status,"
        "attempts=download_items.attempts,error=download_items.error,"
        "http_status=download_items.http_status,retry_after=download_items.retry_after,"
        "next_attempt_at=download_items.next_attempt_at,local_sha256=download_items.local_sha256,"
        "local_bytes=download_items.local_bytes,downloaded_at=download_items.downloaded_at,"
        "last_run_id=excluded.last_run_id,extra_json=excluded.extra_json,"
        "updated_at=excluded.updated_at RETURNING id");
    if (!q)
        return fail(e, n, "SQLite: %s", sqlite3_errmsg(s->db));
    sqlite3_bind_int64(q, 1, source_id);
    text(q, 2, spider);
    text(q, 3, media);
    text(q, 4, logical);
    text(q, 5, string_value(item, "original_name", NULL));
    text(q, 6, file_name);
    text(q, 7, url);
    text(q, 8, string_value(item, "direct_url", NULL));
    text(q, 9, string_value(item, "page_url", NULL));
    text(q, 10, string_value(item, "download_kind", NULL));
    json_text(q, 11, item, jg(item, "options_json") ? "options_json" : "options", 65536);
    text(q, 12, string_value(item, "auth_key", NULL));
    text(q, 13, string_value(item, "content_type", NULL));
    text(q, 14, string_value(item, "file_source", NULL));
    text(q, 15, string_value(item, "format", NULL));
    optional_int(q, 16, item, "format_rank");
    optional_int(q, 17, item, "bytes");
    text(q, 18, string_value(item, "remote_sha1", NULL));
    text(q, 19, string_value(item, "remote_md5", NULL));
    text(q, 20, string_value(item, "remote_crc32", NULL));
    text(q, 21, string_value(item, "expected_sha256", NULL));
    text(q, 22, string_value(item, "content_key", NULL));
    text(q, 23, string_value(item, "role", NULL));
    optional_double(q, 24, item, "sequence");
    sqlite3_bind_int64(q, 25, int_value(item, "part_index", 0));
    sqlite3_bind_int64(q, 26, int_value(item, "part_count", 1));
    text(q, 27, string_value(item, "group_key", NULL));
    sqlite3_bind_int64(q, 28, int_value(item, "score", 0));
    text(q, 29, string_value(item, "decision", "pending"));
    json_text(q, 30, item, "reasons", 65536);
    sqlite3_bind_int64(q, 31, int_value(item, "curated", 0));
    text(q, 32, string_value(item, "notes", NULL));
    text(q, 33, string_value(item, "target_path", NULL));
    text(q, 34, string_value(item, "status", "pending"));
    sqlite3_bind_int64(q, 35, int_value(item, "attempts", 0));
    text(q, 36, string_value(item, "error", NULL));
    optional_int(q, 37, item, "http_status");
    optional_int(q, 38, item, "retry_after");
    optional_int(q, 39, item, "next_attempt_at");
    text(q, 40, string_value(item, "local_sha256", NULL));
    optional_int(q, 41, item, "local_bytes");
    optional_int(q, 42, item, "downloaded_at");
    sqlite3_bind_int64(q, 43, run_id);
    json_text(q, 44, item, jg(item, "extra_json") ? "extra_json" : "extra", 65536);
    sqlite3_bind_int64(q, 45, now);
    sqlite3_bind_int64(q, 46, now);
    int rc = sqlite3_step(q);
    int64_t id = rc == SQLITE_ROW ? sqlite3_column_int64(q, 0) : -1;
    sqlite3_finalize(q);
    if (id < 0)
        return fail(e, n, "Cannot write download item: %s", sqlite3_errmsg(s->db));
    return write_detail(s, id, media, item, e, n);
}

int store_download_scan(Store *s, int64_t run_id, json_object *source, json_object *items,
                        char *e, size_t n)
{
    if (e && n)
        *e = 0;
    if (!s || !source || !json_object_is_type(source, json_type_object) || !items ||
        !json_object_is_type(items, json_type_array))
        return fail(e, n, "Invalid download scan");
    const char *spider = string_value(source, "spider_code", "");
    const char *media = string_value(source, "media_type", "");
    pthread_mutex_lock(&s->mutex);
    int rc = -1;
    int64_t source_id = 0;
    if (sqlite3_exec(s->db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) {
        fail(e, n, "SQLite: %s", sqlite3_errmsg(s->db));
        goto done;
    }
    if (upsert_source(s, source, &source_id, e, n))
        goto rollback;
    for (size_t i = 0; i < json_object_array_length(items); i++) {
        json_object *item = json_object_array_get_idx(items, i);
        if (!item || !json_object_is_type(item, json_type_object)) {
            fail(e, n, "Invalid download item");
            goto rollback;
        }
        if (upsert_item(s, source_id, run_id, spider, media, item, e, n))
            goto rollback;
    }
    if (sqlite3_exec(s->db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
        fail(e, n, "SQLite: %s", sqlite3_errmsg(s->db));
        goto rollback;
    }
    rc = 0;
    goto done;
rollback:
    sqlite3_exec(s->db, "ROLLBACK", NULL, NULL, NULL);
done:
    pthread_mutex_unlock(&s->mutex);
    return rc;
}

json_object *store_download_queue(Store *s, const char *spider_code, const char *media_type,
                                  int limit, char *e, size_t n)
{
    if (!s || !spider_code || !*spider_code) {
        fail(e, n, "Invalid download queue");
        return NULL;
    }
    if (limit <= 0)
        limit = 100;
    if (limit > 10000)
        limit = 10000;
    const char *sql;
    if (media_type && !strcmp(media_type, "music"))
        sql = "SELECT * FROM music_downloads WHERE spider_code=?1 AND decision='include' AND "
              "status IN('pending','failed') AND attempts<3 ORDER BY score DESC,id LIMIT ?2";
    else if (media_type && !strcmp(media_type, "novel"))
        sql = "SELECT * FROM novel_downloads WHERE spider_code=?1 AND decision='include' AND "
              "status IN('pending','failed') AND attempts<3 ORDER BY score DESC,id LIMIT ?2";
    else
        sql = "SELECT d.*,s.identifier AS item_identifier,s.item_url,s.title AS item_title,"
              "s.creator AS item_creator,s.language AS item_language,s.subject,s.collection,s.tags,"
              "s.site_base_url,s.item_type,s.server,s.dir,s.file_count,s.cover_url,s.license,"
              "s.rights,s.region FROM download_items d JOIN download_sources s ON "
              "s.id=d.source_id WHERE d.spider_code=?1 AND (?2='' OR ?2 IS NULL OR "
              "d.media_type=?2) AND d.decision='include' AND d.status IN('pending','failed') AND "
              "d.attempts<3 ORDER BY d.score DESC,d.id LIMIT ?3";
    pthread_mutex_lock(&s->mutex);
    sqlite3_stmt *q = prepare(s, sql);
    json_object *rows = NULL;
    if (!q) {
        fail(e, n, "SQLite: %s", sqlite3_errmsg(s->db));
        goto done;
    }
    text(q, 1, spider_code);
    if (media_type && (!strcmp(media_type, "music") || !strcmp(media_type, "novel")))
        sqlite3_bind_int(q, 2, limit);
    else {
        text(q, 2, media_type);
        sqlite3_bind_int(q, 3, limit);
    }
    rows = json_object_new_array();
    int rc;
    while ((rc = sqlite3_step(q)) == SQLITE_ROW)
        json_object_array_add(rows, row(q));
    if (rc != SQLITE_DONE) {
        fail(e, n, "SQLite: %s", sqlite3_errmsg(s->db));
        json_object_put(rows);
        rows = NULL;
    }
    sqlite3_finalize(q);
done:
    pthread_mutex_unlock(&s->mutex);
    return rows;
}

int store_download_start(Store *s, const char *download_url, int64_t run_id, char *e, size_t n)
{
    if (!s || !download_url || !*download_url)
        return fail(e, n, "Invalid download start");
    pthread_mutex_lock(&s->mutex);
    sqlite3_stmt *q = prepare(
        s, "UPDATE download_items SET status='downloading',attempts=attempts+1,last_run_id=?2,"
           "error='',updated_at=?3 WHERE download_url=?1 AND status IN('pending','failed') AND "
           "attempts<3");
    int rc = -1;
    if (!q)
        fail(e, n, "SQLite: %s", sqlite3_errmsg(s->db));
    else {
        text(q, 1, download_url);
        sqlite3_bind_int64(q, 2, run_id);
        sqlite3_bind_int64(q, 3, now_ms());
        if (sqlite3_step(q) != SQLITE_DONE)
            fail(e, n, "SQLite: %s", sqlite3_errmsg(s->db));
        else if (!sqlite3_changes(s->db))
            fail(e, n, "Download is not queued");
        else
            rc = 0;
        sqlite3_finalize(q);
    }
    pthread_mutex_unlock(&s->mutex);
    return rc;
}

int store_download_finish(Store *s, const char *download_url, const char *status,
                          const char *target_path, const char *local_sha256,
                          int64_t local_bytes, const char *error_text, char *e, size_t n)
{
    if (!s || !download_url || !*download_url || !status ||
        (strcmp(status, "downloaded") && strcmp(status, "skipped") && strcmp(status, "failed")))
        return fail(e, n, "Invalid download result");
    pthread_mutex_lock(&s->mutex);
    sqlite3_stmt *q = prepare(
        s, "UPDATE download_items SET status=?2,target_path=coalesce(?3,target_path),"
           "local_sha256=coalesce(?4,local_sha256),local_bytes=coalesce(?5,local_bytes),error=?6,"
           "downloaded_at=CASE WHEN ?2 IN('downloaded','skipped') THEN ?7 ELSE downloaded_at END,"
           "updated_at=?7 WHERE download_url=?1");
    int rc = -1;
    if (!q)
        fail(e, n, "SQLite: %s", sqlite3_errmsg(s->db));
    else {
        text(q, 1, download_url);
        text(q, 2, status);
        text(q, 3, target_path);
        text(q, 4, local_sha256);
        if (local_bytes < 0)
            sqlite3_bind_null(q, 5);
        else
            sqlite3_bind_int64(q, 5, local_bytes);
        text(q, 6, error_text ? error_text : "");
        sqlite3_bind_int64(q, 7, now_ms());
        if (sqlite3_step(q) != SQLITE_DONE)
            fail(e, n, "SQLite: %s", sqlite3_errmsg(s->db));
        else if (!sqlite3_changes(s->db))
            fail(e, n, "Unknown download URL");
        else
            rc = 0;
        sqlite3_finalize(q);
    }
    pthread_mutex_unlock(&s->mutex);
    return rc;
}

int store_download_decide(Store *s, const char *download_url, const char *decision, int curated,
                          char *e, size_t n)
{
    if (!s || !download_url || !*download_url || !decision ||
        (strcmp(decision, "include") && strcmp(decision, "reject") && strcmp(decision, "pending")))
        return fail(e, n, "Invalid download decision");
    pthread_mutex_lock(&s->mutex);
    sqlite3_stmt *q =
        prepare(s, "UPDATE download_items SET decision=?2,curated=?3,updated_at=?4 "
                   "WHERE download_url=?1");
    int rc = -1;
    if (!q)
        fail(e, n, "SQLite: %s", sqlite3_errmsg(s->db));
    else {
        text(q, 1, download_url);
        text(q, 2, decision);
        sqlite3_bind_int(q, 3, curated ? 1 : 0);
        sqlite3_bind_int64(q, 4, now_ms());
        if (sqlite3_step(q) != SQLITE_DONE)
            fail(e, n, "SQLite: %s", sqlite3_errmsg(s->db));
        else if (!sqlite3_changes(s->db))
            fail(e, n, "Unknown download URL");
        else
            rc = 0;
        sqlite3_finalize(q);
    }
    pthread_mutex_unlock(&s->mutex);
    return rc;
}

json_object *store_download_summary(Store *s, const char *spider_code, char *e, size_t n)
{
    if (!s || !spider_code || !*spider_code) {
        fail(e, n, "Invalid download summary");
        return NULL;
    }
    pthread_mutex_lock(&s->mutex);
    sqlite3_stmt *q = prepare(
        s, "SELECT media_type,status,count(*) AS count,sum(coalesce(local_bytes,bytes,0)) AS bytes "
           "FROM download_items WHERE spider_code=?1 GROUP BY media_type,status ORDER BY "
           "media_type,status");
    json_object *rows = NULL;
    if (!q) {
        fail(e, n, "SQLite: %s", sqlite3_errmsg(s->db));
        goto done;
    }
    text(q, 1, spider_code);
    rows = json_object_new_array();
    int rc;
    while ((rc = sqlite3_step(q)) == SQLITE_ROW)
        json_object_array_add(rows, row(q));
    if (rc != SQLITE_DONE) {
        fail(e, n, "SQLite: %s", sqlite3_errmsg(s->db));
        json_object_put(rows);
        rows = NULL;
    }
    sqlite3_finalize(q);
done:
    pthread_mutex_unlock(&s->mutex);
    return rows;
}
