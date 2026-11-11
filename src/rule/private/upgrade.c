#include "rule/rule.h"
#include <stdio.h>
#include <string.h>

static void string(json_object *o, const char *key, const char *value)
{
    json_object_object_add(o, key, json_object_new_string(value));
}
static void copy(json_object *dst, json_object *src, const char *key)
{
    json_object *value = jg(src, key);
    if (value)
        json_object_object_add(dst, key, flow_clone(value));
}
static json_object *step(json_object *steps, const char *id, const char *type, const char *next)
{
    json_object *s = json_object_new_object();
    string(s, "id", id);
    string(s, "type", type);
    if (next)
        string(s, "next", next);
    json_object_array_add(steps, s);
    return s;
}
static void test(json_object *s, const char *left, const char *op, json_object *right,
                 const char *yes, const char *no)
{
    json_object *t = json_object_new_object();
    string(t, "left", left);
    string(t, "op", op);
    if (right)
        json_object_object_add(t, "right", right);
    json_object_object_add(s, "test", t);
    if (yes)
        string(s, "then", yes);
    if (no)
        string(s, "else", no);
}
static void url_input(json_object *s, const char *url, const char *expression)
{
    json_object *f = json_object_new_object();
    string(f, "url", url);
    json_object_object_add(s, "fields", f);
    string(s, "next_input", expression);
}
json_object *rule_upgrade_v1(json_object *old, char *e, size_t n)
{
    if (ji(old, "schema_version", 1) != 1 || request_validate(old, e, n)) {
        if (e && n && !*e)
            fail(e, n, "Unsupported legacy rule schema");
        return NULL;
    }
    json_object *r = json_object_new_object(), *vars = json_object_new_object(),
                *steps = json_object_new_array(), *defaults = json_object_new_object();
    json_object_object_add(r, "schema_version", json_object_new_int(2));
    copy(r, old, "name");
    copy(r, old, "interval_seconds");
    copy(r, old, "enabled");
    string(r, "entry", "start");
    json_object_object_add(r, "max_steps", json_object_new_int(10000));
    json_object_object_add(r, "vars", vars);
    json_object_object_add(r, "defaults", defaults);
    json_object_object_add(r, "steps", steps);
    const char *opts[] = {"timeout_seconds", "retries", "delay_ms", "max_bytes"};
    for (size_t i = 0; i < sizeof(opts) / sizeof(*opts); i++)
        copy(defaults, old, opts[i]);
    string(vars, "start_url", js(old, "url", ""));
    copy(vars, old, "body");
    json_object *headers = json_object_new_object(), *values = json_object_new_array();
    json_object_object_add(vars, "header_values", values);
    json_object *old_headers = jg(old, "headers");
    if (old_headers) {
        json_object_object_foreach(old_headers, key, value)
        {
            char expr[80];
            snprintf(expr, sizeof(expr), "{{run.header_values.%zu}}",
                     json_object_array_length(values));
            string(headers, key, expr);
            json_object_array_add(values, flow_clone(value));
        }
    }
    url_input(step(steps, "start", "join", "page"), "{{run.start_url}}", "{{steps.start}}");
    json_object *page = step(steps, "page", "request", "rows"), *req = json_object_new_object(),
                *ex = json_object_new_object(), *fields = json_object_new_array(),
                *save_fields = json_object_new_object(), *old_fields = jg(old, "fields");
    json_object_object_add(page, "request", req);
    string(req, "url", "{{input.url}}");
    string(req, "headers_origin", "{{run.start_url}}");
    string(req, "body_origin", "{{run.start_url}}");
    copy(req, old, "method");
    if (jg(old, "body"))
        string(req, "body", "{{run.body}}");
    json_object_object_add(req, "headers", flow_clone(headers));
    /* Legacy retries replayed POST as well as GET. Keep that policy explicit. */
    if (!strcmp(js(old, "method", "GET"), "POST"))
        json_object_object_add(page, "retry_post", json_object_new_boolean(true));
    json_object_object_add(page, "extract", ex);
    copy(ex, old, "format");
    copy(ex, old, "row_selector");
    copy(ex, old, "next_selector");
    json_object_object_add(ex, "fields", fields);
    json_object *rows = step(steps, "rows", "foreach", "more");
    string(rows, "items", "{{steps.page.items}}");
    string(rows, "body", "save");
    char first[64] = "save";
    /* Build the download chain backwards so absent optional fields bypass it. */
    for (size_t i = json_object_array_length(old_fields); i > 0; i--) {
        size_t idx = i - 1;
        json_object *f = json_object_array_get_idx(old_fields, idx);
        if (!jb(f, "download", false))
            continue;
        char id[64], guard[64], expr[80];
        snprintf(id, sizeof(id), "download_%zu", idx);
        snprintf(guard, sizeof(guard), "has_file_%zu", idx);
        snprintf(expr, sizeof(expr), "{{input.field_%zu}}", idx);
        json_object *d = step(steps, id, "download", first);
        string(d, "url", expr);
        string(d, "headers_origin", "{{run.start_url}}");
        json_object_object_add(d, "headers", flow_clone(headers));
        test(step(steps, guard, "condition", NULL), expr, "exists", NULL, id, first);
        snprintf(first, sizeof(first), "%s", guard);
    }
    string(rows, "body", first);
    json_object_put(headers);
    for (size_t i = 0; i < json_object_array_length(old_fields); i++) {
        json_object *f = json_object_array_get_idx(old_fields, i), *internal = flow_clone(f);
        char name[64], expr[80];
        snprintf(name, sizeof(name), "field_%zu", i);
        snprintf(expr, sizeof(expr), "{{input.field_%zu}}", i);
        string(internal, "name", name);
        json_object_array_add(fields, internal);
        string(save_fields, js(f, "name", ""), expr);
    }
    json_object_object_add(step(steps, "save", "emit", NULL), "fields", save_fields);
    /* Retain user column names/order for historical and new CSV exports. */
    json_object_object_add(r, "fields", flow_clone(old_fields));
    test(step(steps, "more", "condition", NULL), "{{steps.page.next_url}}", "exists", NULL, "limit",
         NULL);
    test(step(steps, "limit", "condition", NULL), "{{steps.page.iteration}}", "lt",
         json_object_new_int(ji(old, "max_pages", 1)), "advance", NULL);
    url_input(step(steps, "advance", "join", "page"), "{{steps.page.next_url}}",
              "{{steps.advance}}");
    if (rule_validate(r, e, n)) {
        json_object_put(r);
        return NULL;
    }
    return r;
}
