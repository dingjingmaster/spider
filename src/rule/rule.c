#include "rule.h"
#include <lexbor/css/parser.h>
#include <lexbor/css/selectors/selectors.h>
#include <lexbor/css/selectors/selector.h>
#include <string.h>
#include <ctype.h>
int selector_valid(const char *s)
{
    if (!s || !s[0])
        return 0;
    lxb_css_parser_t *p = lxb_css_parser_create();
    if (!p)
        return -1;
    if (lxb_css_parser_init(p, NULL) != LXB_STATUS_OK) {
        lxb_css_parser_destroy(p, true);
        return -1;
    }
    lxb_css_selector_list_t *list = lxb_css_selectors_parse(p, (const lxb_char_t *)s, strlen(s));
    int ok = list && p->status == LXB_STATUS_OK;
    if (list)
        lxb_css_selector_list_destroy_memory(list);
    lxb_css_parser_destroy(p, true);
    return ok ? 0 : -1;
}
static int bound(json_object *r, const char *key, int def, int low, int high, char *e, size_t n)
{
    json_object *v = jg(r, key);
    if (v && !json_object_is_type(v, json_type_int))
        return fail(e, n, "%s must be an integer", key);
    int64_t x = v ? json_object_get_int64(v) : def;
    return x < low || x > high ? fail(e, n, "%s must be between %d and %d", key, low, high) : 0;
}
int rule_validate(json_object *r, char *e, size_t n)
{
    if (!r || !json_object_is_type(r, json_type_object))
        return fail(e, n, "Rule must be an object");
    const char *name = js(r, "name", "");
    if (!*name || strlen(name) > 160)
        return fail(e, n, "name must contain 1-160 bytes");
    if (!url_valid(js(r, "url", "")))
        return fail(e, n, "url must be http(s), without embedded credentials");
    const char *method = js(r, "method", "GET");
    if (strcmp(method, "GET") && strcmp(method, "POST"))
        return fail(e, n, "method must be GET or POST");
    const char *fmt = js(r, "format", "html");
    bool html = !strcmp(fmt, "html");
    if (!html && strcmp(fmt, "json"))
        return fail(e, n, "format must be html or json");
    if (strcmp(js(r, "backend", "http"), "http"))
        return fail(e, n, "Browser backend is not implemented in this release");
    json_object *headers = jg(r, "headers");
    if (headers) {
        if (!json_object_is_type(headers, json_type_object))
            return fail(e, n, "headers must be an object");
        json_object_object_foreach(headers, k, v)
        {
            if (!*k || strpbrk(k, ":\r\n") || !json_object_is_type(v, json_type_string) ||
                strpbrk(json_object_get_string(v), "\r\n"))
                return fail(e, n, "Invalid HTTP header");
        }
    }
    if (jg(r, "body") && !json_object_is_type(jg(r, "body"), json_type_string))
        return fail(e, n, "body must be a string");
    const char *row = js(r, "row_selector", "");
    if (html && selector_valid(row))
        return fail(e, n, "Invalid row CSS selector");
    if (!html && *row && *row != '/')
        return fail(e, n, "JSON row_selector must be a JSON Pointer starting with /");
    const char *next = js(r, "next_selector", "");
    if (html && selector_valid(next))
        return fail(e, n, "Invalid next CSS selector");
    if (!html && *next && *next != '/')
        return fail(e, n, "JSON next_selector must be a JSON Pointer");
    json_object *fields = jg(r, "fields");
    if (!fields || !json_object_is_type(fields, json_type_array) ||
        !json_object_array_length(fields) || json_object_array_length(fields) > 64)
        return fail(e, n, "fields must contain 1-64 field definitions");
    for (size_t i = 0; i < json_object_array_length(fields); i++) {
        json_object *f = json_object_array_get_idx(fields, i);
        const char *key = js(f, "name", "");
        if (!*key || strlen(key) > 80)
            return fail(e, n, "Invalid field name");
        for (size_t j = 0; j < i; j++)
            if (!strcmp(key, js(json_object_array_get_idx(fields, j), "name", "")))
                return fail(e, n, "Duplicate field name: %s", key);
        const char *sel = js(f, "selector", "");
        if (html && selector_valid(sel))
            return fail(e, n, "Invalid selector for field %s", key);
        if (!html && *sel && *sel != '/')
            return fail(e, n, "Field %s requires JSON Pointer syntax", key);
        const char *type = js(f, "type", "string");
        if (strcmp(type, "string") && strcmp(type, "number") && strcmp(type, "integer") &&
            strcmp(type, "boolean"))
            return fail(e, n, "Unknown type for field %s", key);
        const char *attr = js(f, "attribute", "");
        if (strlen(attr) > 80)
            return fail(e, n, "Attribute too long");
        const char *bools[] = {"required", "download"};
        for (size_t b = 0; b < 2; b++) {
            json_object *v = jg(f, bools[b]);
            if (v && !json_object_is_type(v, json_type_boolean))
                return fail(e, n, "%s must be boolean", bools[b]);
        }
    }
    if (bound(r, "max_pages", 1, 1, 1000, e, n) ||
        bound(r, "interval_seconds", 0, 0, 31536000, e, n) ||
        bound(r, "delay_ms", 300, 0, 60000, e, n) || bound(r, "retries", 2, 0, 5, e, n) ||
        bound(r, "timeout_seconds", 30, 1, 300, e, n) ||
        bound(r, "max_bytes", 4194304, 1, 104857600, e, n))
        return -1;
    if (jg(r, "enabled") && !json_object_is_type(jg(r, "enabled"), json_type_boolean))
        return fail(e, n, "enabled must be boolean");
    return 0;
}
