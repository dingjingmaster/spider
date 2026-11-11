#include "storage/private/storage_internal.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
static int export_error(char *e, size_t n, int status, const char *message)
{
    fail(e, n, "%s", message);
    return status;
}
static void csv_cell(FILE *f, const char *s)
{
    fputc('"', f);
    if (*s == '=' || *s == '+' || *s == '-' || *s == '@' || *s == '\t' || *s == '\r')
        fputc('\'', f);
    for (; *s; s++) {
        if (*s == '"')
            fputc('"', f);
        fputc(*s, f);
    }
    fputc('"', f);
}
static bool csv_fields_valid(json_object *fields)
{
    if (!fields || !json_object_is_type(fields, json_type_array) ||
        !json_object_array_length(fields))
        return false;
    for (size_t i = 0; i < json_object_array_length(fields); i++) {
        json_object *field = json_object_array_get_idx(fields, i);
        if (!field || !json_object_is_type(field, json_type_object))
            return false;
        json_object *name = jg(field, "name");
        if (!name || !json_object_is_type(name, json_type_string) || !*json_object_get_string(name))
            return false;
    }
    return true;
}
int store_export(Store *s, int64_t id, bool csv, char **out, size_t *out_size, char *e, size_t n)
{
    json_object *run = store_run(s, id);
    if (!run)
        return export_error(e, n, 404, "Run not found");
    char *body = NULL;
    size_t size = 0;
    FILE *f = open_memstream(&body, &size);
    if (!f) {
        json_object_put(run);
        return export_error(e, n, 500, "Cannot create export");
    }
    pthread_mutex_lock(&s->mutex);
    json_object *snapshot = jg(run, "snapshot");
    json_object *fields = jg(snapshot, "fields"), *derived_fields = NULL;
    bool workflow = !strcmp(js(snapshot, "implementation", "workflow"), "workflow");
    bool explicit_fields = workflow && json_object_object_get_ex(snapshot, "fields", &fields);
    if (csv && explicit_fields && !csv_fields_valid(fields)) {
        pthread_mutex_unlock(&s->mutex);
        fclose(f);
        free(body);
        json_object_put(run);
        return export_error(
            e, n, 422, "Workflow fields must be a nonempty array of objects with name strings");
    }
    /* Explicit export fields select/order columns; otherwise infer emitted record keys. */
    if (csv && !explicit_fields) {
        derived_fields = json_object_new_array();
        sqlite3_stmt *columns = NULL;
        int cr =
            sqlite3_prepare_v2(s->db,
                               "SELECT DISTINCT json_each.key FROM items,json_each(items.data) "
                               "WHERE items.run_id=? ORDER BY json_each.key",
                               -1, &columns, NULL);
        if (cr == SQLITE_OK) {
            sqlite3_bind_int64(columns, 1, id);
            while ((cr = sqlite3_step(columns)) == SQLITE_ROW) {
                json_object *field = json_object_new_object();
                json_object_object_add(
                    field, "name",
                    json_object_new_string((const char *)sqlite3_column_text(columns, 0)));
                json_object_array_add(derived_fields, field);
            }
        }
        sqlite3_finalize(columns);
        if (cr != SQLITE_DONE) {
            pthread_mutex_unlock(&s->mutex);
            fclose(f);
            free(body);
            json_object_put(derived_fields);
            json_object_put(run);
            return export_error(e, n, 500, "Cannot derive export columns");
        }
        fields = derived_fields;
    }
    if (csv) {
        for (size_t i = 0; i < json_object_array_length(fields); i++) {
            if (i)
                fputc(',', f);
            csv_cell(f, js(json_object_array_get_idx(fields, i), "name", ""));
        }
        fputs("\r\n", f);
    } else
        fputc('[', f);
    sqlite3_stmt *q = NULL;
    int rc = sqlite3_prepare_v2(s->db, "SELECT data FROM items WHERE run_id=? ORDER BY id", -1, &q,
                                NULL);
    size_t count = 0;
    bool too_large = false;
    if (rc == SQLITE_OK) {
        sqlite3_bind_int64(q, 1, id);
        while ((rc = sqlite3_step(q)) == SQLITE_ROW) {
            const char *text = (const char *)sqlite3_column_text(q, 0);
            if (csv) {
                json_object *item = json_tokener_parse(text);
                for (size_t i = 0; i < json_object_array_length(fields); i++) {
                    if (i)
                        fputc(',', f);
                    json_object *v = jg(item, js(json_object_array_get_idx(fields, i), "name", ""));
                    csv_cell(f, v ? json_object_get_string(v) : "");
                }
                fputs("\r\n", f);
                json_object_put(item);
            } else {
                if (count)
                    fputc(',', f);
                fputs(text, f);
            }
            count++;
            if (ftell(f) > 33554432) {
                too_large = true;
                break;
            }
        }
    }
    sqlite3_finalize(q);
    pthread_mutex_unlock(&s->mutex);
    if (!csv)
        fputc(']', f);
    int io = fclose(f);
    if (derived_fields)
        json_object_put(derived_fields);
    json_object_put(run);
    if (io || too_large || rc != SQLITE_DONE) {
        free(body);
        return export_error(e, n, too_large ? 413 : 500,
                            too_large ? "Export exceeds 32 MiB; use paginated results API"
                                      : "Export failed");
    }
    *out = body;
    *out_size = size;
    return 0;
}
