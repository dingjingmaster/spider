#include "parser.h"
#include "rule/rule.h"
#include <lexbor/html/html.h>
#include <lexbor/dom/dom.h>
#include <lexbor/css/parser.h>
#include <lexbor/css/selectors/selectors.h>
#include <lexbor/selectors/selectors.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    lxb_dom_node_t **nodes;
    size_t len, cap;
    bool first;
} Nodes;
static lxb_status_t collect_node(lxb_dom_node_t *node, lxb_css_selector_specificity_t specificity,
                                 void *context)
{
    (void)specificity;
    Nodes *n = context;
    if (n->len >= 10000)
        return LXB_STATUS_ERROR;
    if (n->len == n->cap) {
        size_t cap = n->cap ? n->cap * 2 : 16;
        void *p = realloc(n->nodes, cap * sizeof(*n->nodes));
        if (!p)
            return LXB_STATUS_ERROR;
        n->nodes = p;
        n->cap = cap;
    }
    n->nodes[n->len++] = node;
    return n->first ? LXB_STATUS_STOP : LXB_STATUS_OK;
}
static int select_nodes(lxb_dom_node_t *root, const char *css, Nodes *out)
{
    lxb_css_parser_t *p = lxb_css_parser_create();
    lxb_selectors_t *s = lxb_selectors_create();
    int rc = -1;
    if (!p || !s)
        goto done;
    if (lxb_css_parser_init(p, NULL) != LXB_STATUS_OK || lxb_selectors_init(s) != LXB_STATUS_OK)
        goto done;
    lxb_css_selector_list_t *list =
        lxb_css_selectors_parse(p, (const lxb_char_t *)css, strlen(css));
    if (list && p->status == LXB_STATUS_OK) {
        lxb_status_t status = lxb_selectors_find(s, root, list, collect_node, out);
        if (status == LXB_STATUS_OK || status == LXB_STATUS_STOP)
            rc = 0;
    }
    if (list)
        lxb_css_selector_list_destroy_memory(list);
done:
    if (s)
        lxb_selectors_destroy(s, true);
    if (p)
        lxb_css_parser_destroy(p, true);
    return rc;
}
static char *node_value(lxb_dom_node_t *node, const char *attr)
{
    size_t len = 0;
    const lxb_char_t *text = NULL;
    if (*attr) {
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT)
            text = lxb_dom_element_get_attribute(lxb_dom_interface_element(node),
                                                 (const lxb_char_t *)attr, strlen(attr), &len);
    } else
        text = lxb_dom_node_text_content(node, &len);
    return text ? trim_copy((const char *)text, len) : NULL;
}
static int convert_value(json_object *field, json_object *raw, bool parse_text, json_object **value,
                         char *e, size_t n)
{
    const char *name = js(field, "name", ""), *type = js(field, "type", "string");
    if (!raw || json_object_is_type(raw, json_type_null)) {
        if (jb(field, "required", false))
            return fail(e, n, "Required field missing: %s", name);
        *value = NULL;
        return 0;
    }
    if (!strcmp(type, "object") || !strcmp(type, "array") || !strcmp(type, "json")) {
        json_object *parsed = NULL;
        if (parse_text && json_object_is_type(raw, json_type_string)) {
            if (json_value_parse(json_object_get_string(raw), &parsed, e, n))
                return fail(e, n, "Field %s is not valid JSON", name);
        } else
            parsed = flow_clone(raw);
        if ((!strcmp(type, "object") && !json_object_is_type(parsed, json_type_object)) ||
            (!strcmp(type, "array") && !json_object_is_type(parsed, json_type_array)) ||
            (!parsed && jb(field, "required", false))) {
            json_object_put(parsed);
            return fail(e, n, "Field %s does not match type %s", name, type);
        }
        *value = parsed;
        return 0;
    }
    const char *str = json_object_get_string(raw);
    char *s = trim_copy(str ? str : "", str ? strlen(str) : 0);
    if (!s)
        return fail(e, n, "Out of memory");
    if (jb(field, "required", false) && !*s) {
        free(s);
        return fail(e, n, "Required field empty: %s", name);
    }
    int rc = 0;
    char *end = NULL;
    errno = 0;
    if (!strcmp(type, "number")) {
        double x = strtod(s, &end);
        if (errno || end == s || *end || !isfinite(x))
            rc = fail(e, n, "Field %s is not a finite number", name);
        else
            *value = json_object_new_double(x);
    } else if (!strcmp(type, "integer")) {
        long long x = strtoll(s, &end, 10);
        if (errno || end == s || *end)
            rc = fail(e, n, "Field %s is not an integer", name);
        else
            *value = json_object_new_int64(x);
    } else if (!strcmp(type, "boolean")) {
        if (!strcmp(s, "true") || !strcmp(s, "1"))
            *value = json_object_new_boolean(true);
        else if (!strcmp(s, "false") || !strcmp(s, "0"))
            *value = json_object_new_boolean(false);
        else
            rc = fail(e, n, "Field %s is not boolean", name);
    } else
        *value = json_object_new_string(s);
    free(s);
    return rc;
}
int extract_document(json_object *rule, const char *body, const char *url, json_object **items,
                     char **next, char *e, size_t n)
{
    *items = NULL;
    *next = NULL;
    json_object *out = json_object_new_array(), *root = NULL;
    lxb_html_document_t *doc = NULL;
    Nodes rows = {0};
    int rc = -1;
    json_object *fields = jg(rule, "fields");
    bool html = !strcmp(js(rule, "format", "html"), "html");
    const char *row_sel = js(rule, "row_selector", "");
    if (html) {
        doc = lxb_html_document_create();
        if (!doc ||
            lxb_html_document_parse(doc, (const lxb_char_t *)body, strlen(body)) != LXB_STATUS_OK) {
            fail(e, n, "Could not parse HTML");
            goto done;
        }
        if (*row_sel) {
            if (select_nodes(lxb_dom_interface_node(doc), row_sel, &rows)) {
                fail(e, n, "Could not select HTML rows");
                goto done;
            }
        } else {
            rows.nodes = malloc(sizeof(*rows.nodes));
            if (!rows.nodes)
                goto done;
            rows.nodes[0] = lxb_dom_interface_node(doc);
            rows.len = 1;
        }
    } else {
        if (json_value_parse(body, &root, e, n))
            goto done;
    }
    json_object *json_rows = root;
    if (!html && *row_sel && json_pointer_get(root, row_sel, &json_rows)) {
        fail(e, n, "JSON row path not found: %s", row_sel);
        goto done;
    }
    size_t count = html ? rows.len
                        : (json_object_is_type(json_rows, json_type_array)
                               ? json_object_array_length(json_rows)
                               : 1);
    if (count > 10000) {
        fail(e, n, "A page may contain at most 10000 rows");
        goto done;
    }
    for (size_t i = 0; i < count; i++) {
        json_object *item = json_object_new_object(),
                    *row = html ? NULL
                                : (json_object_is_type(json_rows, json_type_array)
                                       ? json_object_array_get_idx(json_rows, i)
                                       : json_rows);
        for (size_t j = 0; j < json_object_array_length(fields); j++) {
            json_object *f = json_object_array_get_idx(fields, j), *raw = NULL, *v = NULL;
            char *s = NULL;
            const char *sel = js(f, "selector", "");
            if (html) {
                Nodes found = {.first = true};
                lxb_dom_node_t *node = rows.nodes[i];
                if (*sel) {
                    if (select_nodes(node, sel, &found)) {
                        free(found.nodes);
                        json_object_put(item);
                        fail(e, n, "Field selector failed");
                        goto done;
                    }
                    node = found.len ? found.nodes[0] : NULL;
                }
                if (node)
                    s = node_value(node, js(f, "attribute", ""));
                free(found.nodes);
                if (s) {
                    raw = json_object_new_string(s);
                    free(s);
                }
            } else {
                if (!*sel)
                    raw = row;
                else if (json_pointer_get(row, sel, &raw))
                    raw = NULL;
                if (raw)
                    json_object_get(raw);
            }
            if (convert_value(f, raw, html, &v, e, n)) {
                if (raw)
                    json_object_put(raw);
                json_object_put(item);
                goto done;
            }
            if (raw)
                json_object_put(raw);
            if (v && jb(f, "download", false)) {
                char *resolved = url_resolve(url, json_object_get_string(v));
                json_object_put(v);
                if (!resolved) {
                    json_object_put(item);
                    fail(e, n, "Invalid download URL");
                    goto done;
                }
                v = json_object_new_string(resolved);
                free(resolved);
            }
            json_object_object_add(item, js(f, "name", ""), v);
        }
        json_object_array_add(out, item);
    }
    const char *ns = js(rule, "next_selector", "");
    char *link = NULL;
    if (*ns) {
        if (html) {
            Nodes found = {.first = true};
            if (select_nodes(lxb_dom_interface_node(doc), ns, &found)) {
                free(found.nodes);
                fail(e, n, "Next selector failed");
                goto done;
            }
            if (found.len)
                link = node_value(found.nodes[0], "href");
            free(found.nodes);
        } else {
            json_object *v = NULL;
            if (!json_pointer_get(root, ns, &v) && v && json_object_is_type(v, json_type_string))
                link = strdup(json_object_get_string(v));
        }
    }
    if (link && *link) {
        *next = url_resolve(url, link);
        if (!*next) {
            free(link);
            fail(e, n, "Invalid next page URL");
            goto done;
        }
    }
    free(link);
    *items = out;
    out = NULL;
    rc = 0;
done:
    if (out)
        json_object_put(out);
    if (root)
        json_object_put(root);
    free(rows.nodes);
    if (doc)
        lxb_html_document_destroy(doc);
    if (rc) {
        free(*next);
        *next = NULL;
    }
    return rc;
}
