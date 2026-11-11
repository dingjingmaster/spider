/* Catalog sync must preserve identities and history, quarantine legacy jobs,
 * and roll back all changes if any version write fails. */
#include "crawler/crawler.h"
#include "storage/private/storage_internal.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
static int run(CrawlerContext *ctx, char *e, size_t n)
{
    (void)ctx;
    (void)e;
    (void)n;
    return 0;
}
static CrawlerRegistry *registry(const char *name, bool second)
{
    char e[SP_ERROR] = {0};
    CrawlerRegistry *r = crawler_create();
    assert(r);
    CrawlerDefinition definition = {.id = "native", .name = "Native", .run = run};
    assert(!crawler_register(r, &definition, e, sizeof(e)));
    json_object *config = json_object_new_object();
    json_object_object_add(config, "name", json_object_new_string(name));
    json_object_object_add(config, "interval_seconds", json_object_new_int(60));
    assert(!crawler_add(r, "site-a", "native", config, e, sizeof(e)));
    assert(crawler_add(r, "site-a", "native", config, e, sizeof(e)) < 0);
    assert(crawler_add(r, "unknown", "missing", config, e, sizeof(e)) < 0);
    if (second) {
        json_object_object_add(config, "name", json_object_new_string("B"));
        assert(!crawler_add(r, "site-b", "native", config, e, sizeof(e)));
    }
    json_object_object_add(config, "name", json_object_new_string("caller edit"));
    json_object_put(config);
    return r;
}
static int64_t scalar(Store *s, const char *sql)
{
    sqlite3_stmt *q = NULL;
    assert(sqlite3_prepare_v2(s->db, sql, -1, &q, NULL) == SQLITE_OK);
    assert(sqlite3_step(q) == SQLITE_ROW);
    int64_t value = sqlite3_column_int64(q, 0);
    sqlite3_finalize(q);
    return value;
}
int main(void)
{
    char path[] = "/tmp/spider-catalog-XXXXXX", e[SP_ERROR] = {0};
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    Store *s = store_create(path, e, sizeof(e));
    assert(s);
    json_object *legacy =
        json_tokener_parse("{\"name\":\"old web task\",\"enabled\":true,\"interval_seconds\":1}");
    int64_t old = store_save_spider(s, 0, legacy, e, sizeof(e));
    assert(old > 0);
    int64_t oldrun = store_start_run(s, old, e, sizeof(e));
    assert(oldrun > 0);
    json_object_put(legacy);
    CrawlerRegistry *r = registry("A", true);
    assert(!crawler_sync(r, s, e, sizeof(e)));
    json_object *rows = store_code_spiders(s, e, sizeof(e));
    assert(rows && json_object_array_length(rows) == 2);
    json_object *a = json_object_array_get_idx(rows, 0);
    int64_t id = json_object_get_int64(jg(a, "id"));
    assert(!strcmp(js(a, "code_id", ""), "site-a") && !strcmp(js(a, "name", ""), "A"));
    json_object_put(rows);
    json_object *oldrow = store_run(s, oldrun);
    assert(!strcmp(js(oldrow, "status", ""), "cancelled"));
    json_object_put(oldrow);
    assert(scalar(s, "SELECT enabled FROM spiders WHERE id=1") == 0);
    assert(!store_next_due(s, id, 123456789));
    int64_t version = scalar(s, "SELECT version FROM spiders WHERE id=(SELECT spider_id FROM "
                                "code_spiders WHERE code_id='site-a')");
    int64_t runid = store_start_run(s, id, e, sizeof(e));
    assert(runid > 0);
    json_object *claimed = store_claim_run(s, NULL, 0, e, sizeof(e));
    assert(claimed);
    json_object_put(claimed);
    assert(!store_finish_run(s, runid, "succeeded", ""));
    crawler_destroy(r);
    store_destroy(s);
    s = store_create(path, e, sizeof(e));
    assert(s);
    r = registry("A", true);
    assert(!crawler_sync(r, s, e, sizeof(e)));
    rows = store_code_spiders(s, e, sizeof(e));
    assert(json_object_get_int64(jg(json_object_array_get_idx(rows, 0), "id")) == id);
    json_object_put(rows);
    assert(scalar(s, "SELECT next_run FROM spiders WHERE id=(SELECT spider_id FROM code_spiders "
                     "WHERE code_id='site-a')") == 123456789);
    assert(scalar(s, "SELECT version FROM spiders WHERE id=(SELECT spider_id FROM code_spiders "
                     "WHERE code_id='site-a')") == version);
    assert(scalar(s, "SELECT count(*) FROM runs WHERE status='succeeded'") == 1);
    crawler_destroy(r);
    r = registry("Changed", false);
    assert(!sqlite3_exec(s->db,
                         "CREATE TRIGGER reject_catalog_version BEFORE INSERT ON rule_versions "
                         "BEGIN SELECT RAISE(ABORT,'reject catalog'); END",
                         NULL, NULL, NULL));
    assert(crawler_sync(r, s, e, sizeof(e)) < 0);
    rows = store_code_spiders(s, e, sizeof(e));
    assert(rows && json_object_array_length(rows) == 2);
    assert(!strcmp(js(json_object_array_get_idx(rows, 0), "name", ""), "A"));
    json_object_put(rows);
    assert(!sqlite3_exec(s->db, "DROP TRIGGER reject_catalog_version", NULL, NULL, NULL));
    assert(!crawler_sync(r, s, e, sizeof(e)));
    rows = store_code_spiders(s, e, sizeof(e));
    assert(json_object_array_length(rows) == 1);
    assert(!strcmp(js(json_object_array_get_idx(rows, 0), "name", ""), "Changed"));
    json_object_put(rows);
    assert(scalar(s, "SELECT enabled FROM spiders WHERE id=(SELECT spider_id FROM code_spiders "
                     "WHERE code_id='site-b')") == 0);
    crawler_destroy(r);
    r = registry("Changed", true);
    assert(!crawler_sync(r, s, e, sizeof(e)));
    assert(scalar(s, "SELECT enabled FROM spiders WHERE id=(SELECT spider_id FROM code_spiders "
                     "WHERE code_id='site-b')") == 1);
    crawler_destroy(r);
    r = crawler_create();
    assert(!crawler_sync(r, s, e, sizeof(e)));
    rows = store_code_spiders(s, e, sizeof(e));
    assert(rows && !json_object_array_length(rows));
    json_object_put(rows);
    assert(scalar(s, "SELECT count(*) FROM runs WHERE status='succeeded'") == 1);
    crawler_destroy(r);
    store_destroy(s);
    unlink(path);
    puts("code crawler catalog passed");
    return 0;
}
