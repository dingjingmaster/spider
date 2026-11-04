#include "workflow.h"
#include "rule.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

json_object *flow_clone(json_object *v)
{
    json_object *copy = NULL;
    if (v && json_object_deep_copy(v, &copy, NULL) != 0)
        return NULL;
    return copy;
}
json_object *flow_step(json_object *r, const char *id)
{
    json_object *steps = jg(r, "steps");
    if (!steps || !json_object_is_type(steps, json_type_array))
        return NULL;
    for (size_t i = 0; i < json_object_array_length(steps); i++) {
        json_object *s = json_object_array_get_idx(steps, i);
        if (!strcmp(js(s, "id", ""), id))
            return s;
    }
    return NULL;
}
static bool identifier(const char *s)
{
    if (!s || !s[0] || strlen(s) > 64 || !(isalpha((unsigned char)*s) || *s == '_'))
        return false;
    for (; *s; s++)
        if (!isalnum((unsigned char)*s) && *s != '_' && *s != '-')
            return false;
    return true;
}
static int template_syntax(const char *s, char *e, size_t n)
{
    const char *p = s;
    while (*p) {
        const char *a = strstr(p, "{{"), *bad = strstr(p, "}}");
        if (!a) {
            if (bad)
                return fail(e, n, "Unmatched template delimiter");
            break;
        }
        if (bad && bad < a)
            return fail(e, n, "Unmatched template delimiter");
        const char *b = strstr(a + 2, "}}");
        if (!b)
            return fail(e, n, "Unclosed template");
        char *path = trim_copy(a + 2, (size_t)(b - a - 2));
        if (!path)
            return fail(e, n, "Out of memory");
        bool valid = !strncmp(path, "input", 5) && (!path[5] || path[5] == '.');
        valid |= !strncmp(path, "run", 3) && (!path[3] || path[3] == '.');
        valid |= !strncmp(path, "steps", 5) && (!path[5] || path[5] == '.');
        for (char *c = path; *c; c++)
            if (!isalnum((unsigned char)*c) && *c != '_' && *c != '-' && *c != '.')
                valid = false;
        free(path);
        if (!valid)
            return fail(e, n, "Template roots must be run, input or steps");
        p = b + 2;
    }
    return 0;
}
static int validate_templates(json_object *v, int depth, char *e, size_t n)
{
    if (depth > 32)
        return fail(e, n, "Workflow JSON nesting exceeds 32");
    if (json_object_is_type(v, json_type_string)) {
        const char *s = json_object_get_string(v);
        if (strlen(s) != (size_t)json_object_get_string_len(v))
            return fail(e, n, "NUL byte in workflow string");
        return template_syntax(s, e, n);
    }
    if (json_object_is_type(v, json_type_object)) {
        json_object_object_foreach(v, k, child)
        {
            (void)k;
            if (validate_templates(child, depth + 1, e, n))
                return -1;
        }
    }
    if (json_object_is_type(v, json_type_array))
        for (size_t i = 0; i < json_object_array_length(v); i++)
            if (validate_templates(json_object_array_get_idx(v, i), depth + 1, e, n))
                return -1;
    return 0;
}
static int reference(json_object *r, json_object *s, const char *key, bool required, char *e,
                     size_t n)
{
    json_object *v = NULL;
    bool has = json_object_object_get_ex(s, key, &v);
    if (!has || !v)
        return required ? fail(e, n, "Step %s requires %s", js(s, "id", ""), key) : 0;
    if (!json_object_is_type(v, json_type_string) || !flow_step(r, json_object_get_string(v)))
        return fail(e, n, "Unknown step reference in %s.%s", js(s, "id", ""), key);
    return 0;
}
static int config_bounds(json_object *d, char *e, size_t n)
{
    const char *keys[] = {"retries", "delay_ms", "timeout_seconds", "max_bytes"};
    int low[] = {0, 0, 1, 1}, high[] = {5, 60000, 300, 104857600};
    for (size_t i = 0; i < 4; i++) {
        json_object *v = NULL;
        if (json_object_object_get_ex(d, keys[i], &v) &&
            (!v || !json_object_is_type(v, json_type_int) || json_object_get_int64(v) < low[i] ||
             json_object_get_int64(v) > high[i]))
            return fail(e, n, "Invalid %s", keys[i]);
    }
    return 0;
}
int flow_validate(json_object *r, char *e, size_t n)
{
    if (!*js(r, "name", "") || strlen(js(r, "name", "")) > 160)
        return fail(e, n, "Workflow requires name (1-160 bytes)");
    json_object *steps = jg(r, "steps"), *defaults = jg(r, "defaults"), *vars = jg(r, "vars");
    if (!steps || !json_object_is_type(steps, json_type_array) ||
        !json_object_array_length(steps) || json_object_array_length(steps) > 256)
        return fail(e, n, "steps must contain 1-256 definitions");
    if (defaults && !json_object_is_type(defaults, json_type_object))
        return fail(e, n, "defaults must be an object");
    if (vars && !json_object_is_type(vars, json_type_object))
        return fail(e, n, "vars must be an object");
    if (config_bounds(defaults, e, n))
        return -1;
    json_object *limit = jg(r, "max_steps");
    if (limit && (!json_object_is_type(limit, json_type_int) || json_object_get_int64(limit) < 1 ||
                  json_object_get_int64(limit) > 10000))
        return fail(e, n, "max_steps must be 1-10000");
    json_object *interval = jg(r, "interval_seconds");
    if (interval &&
        (!json_object_is_type(interval, json_type_int) || json_object_get_int64(interval) < 0 ||
         json_object_get_int64(interval) > 31536000))
        return fail(e, n, "Invalid interval_seconds");
    if (jg(r, "enabled") && !json_object_is_type(jg(r, "enabled"), json_type_boolean))
        return fail(e, n, "enabled must be boolean");
    if (!flow_step(r, js(r, "entry", "")))
        return fail(e, n, "entry must reference an existing step");
    for (size_t i = 0; i < json_object_array_length(steps); i++) {
        json_object *s = json_object_array_get_idx(steps, i);
        const char *id = js(s, "id", ""), *type = js(s, "type", ""),
                   *policy = js(s, "on_error", "fail");
        if (!identifier(id))
            return fail(e, n, "Invalid step id");
        for (size_t j = 0; j < i; j++)
            if (!strcmp(id, js(json_object_array_get_idx(steps, j), "id", "")))
                return fail(e, n, "Duplicate step id: %s", id);
        if (strcmp(policy, "fail") && strcmp(policy, "skip") && strcmp(policy, "continue"))
            return fail(e, n, "Invalid on_error for %s", id);
        if (reference(r, s, "next", false, e, n) || config_bounds(s, e, n))
            return -1;
        /* Only rendered values are templates; vars, selectors and names are literal data. */
        const char *templated[] = {"request", "next_input", "items",   "test",
                                   "fields",  "url",        "headers", "headers_origin"};
        for (size_t k = 0; k < sizeof(templated) / sizeof(*templated); k++)
            if (validate_templates(jg(s, templated[k]), 0, e, n))
                return -1;
        if (!strcmp(type, "request")) {
            json_object *req = jg(s, "request");
            if (!req || !json_object_is_type(req, json_type_object) || !*js(req, "url", ""))
                return fail(e, n, "request step %s requires request.url", id);
            const char *method = js(req, "method", "GET");
            if (strcmp(method, "GET") && strcmp(method, "POST"))
                return fail(e, n, "request method must be GET or POST");
            if (!strstr(js(req, "url", ""), "{{") && !url_valid(js(req, "url", "")))
                return fail(e, n, "Invalid request URL in %s", id);
            if (jg(req, "query") && !json_object_is_type(jg(req, "query"), json_type_object))
                return fail(e, n, "query must be an object");
            if (jg(req, "headers") && !json_object_is_type(jg(req, "headers"), json_type_object))
                return fail(e, n, "headers must be an object");
            if (jg(req, "body") && jg(req, "json"))
                return fail(e, n, "Use body or json, not both");
            if (jg(req, "body") && !json_object_is_type(jg(req, "body"), json_type_string))
                return fail(e, n, "body must be a string");
            if (jg(s, "retry_post") && !json_object_is_type(jg(s, "retry_post"), json_type_boolean))
                return fail(e, n, "retry_post must be boolean");
            json_object *ex = jg(s, "extract");
            if (ex) {
                if (!json_object_is_type(ex, json_type_object))
                    return fail(e, n, "extract must be an object");
                json_object *flat = flow_clone(ex);
                json_object_object_add(flat, "name", json_object_new_string(id));
                json_object_object_add(flat, "url", json_object_new_string("https://example.com/"));
                int rc = request_validate(flat, e, n);
                json_object_put(flat);
                if (rc)
                    return -1;
            }
        } else if (!strcmp(type, "foreach")) {
            if (!json_object_object_get_ex(s, "items", &limit) ||
                reference(r, s, "body", true, e, n))
                return fail(e, n, "foreach requires items and valid body");
        } else if (!strcmp(type, "parallel")) {
            json_object *branches = jg(s, "branches");
            if (!branches || !json_object_is_type(branches, json_type_object) ||
                !json_object_object_length(branches) || json_object_object_length(branches) > 64)
                return fail(e, n, "parallel branches must be a nonempty object (max 64)");
            json_object_object_foreach(branches, k, v)
            {
                if (!identifier(k) || !json_object_is_type(v, json_type_string) ||
                    !flow_step(r, json_object_get_string(v)))
                    return fail(e, n, "Invalid parallel branch");
            }
        } else if (!strcmp(type, "condition")) {
            json_object *test = jg(s, "test");
            const char *op = js(test, "op", "exists");
            const char *ops[] = {"exists", "eq", "ne", "gt", "gte", "lt", "lte", "contains"};
            bool valid = false;
            for (size_t k = 0; k < 8; k++)
                if (!strcmp(op, ops[k]))
                    valid = true;
            if (!test || !json_object_is_type(test, json_type_object) || !valid ||
                reference(r, s, "then", false, e, n) || reference(r, s, "else", false, e, n))
                return fail(e, n, "Invalid condition %s", id);
        } else if (!strcmp(type, "download")) {
            if (!*js(s, "url", ""))
                return fail(e, n, "download requires url");
            if (jg(s, "headers") && !json_object_is_type(jg(s, "headers"), json_type_object))
                return fail(e, n, "download headers must be an object");
            if (jg(s, "headers_origin") &&
                !json_object_is_type(jg(s, "headers_origin"), json_type_string))
                return fail(e, n, "download headers_origin must be a string");
        } else if (!strcmp(type, "emit") || !strcmp(type, "join")) {
            if (!jg(s, "fields") || !json_object_is_type(jg(s, "fields"), json_type_object))
                return fail(e, n, "%s requires fields object", id);
        } else
            return fail(e, n, "Unknown step type: %s", type);
    }
    return 0;
}
static json_object *lookup(json_object *ctx, const char *path, bool *found)
{
    *found = true;
    char *copy = strdup(path);
    if (!copy)
        return NULL;
    json_object *cur = ctx;
    char *save = NULL;
    for (char *part = strtok_r(copy, ".", &save); part; part = strtok_r(NULL, ".", &save)) {
        if (!cur) {
            *found = false;
            break;
        }
        if (json_object_is_type(cur, json_type_array)) {
            char *end = NULL;
            long idx = strtol(part, &end, 10);
            if (!*part || *end || idx < 0 || (size_t)idx >= json_object_array_length(cur)) {
                cur = NULL;
                *found = false;
                break;
            }
            cur = json_object_array_get_idx(cur, (size_t)idx);
        } else if (!json_object_object_get_ex(cur, part, &cur)) {
            *found = false;
            cur = NULL;
            break;
        }
    }
    free(copy);
    return cur;
}
static int charge(size_t bytes, size_t *remaining, char *e, size_t n)
{
    if (bytes > *remaining)
        return fail(e, n, "Template output exceeds 4 MiB");
    *remaining -= bytes;
    return 0;
}
static int charge_value(json_object *v, size_t *remaining, char *e, size_t n)
{
    size_t bytes = 0;
    json_object_to_json_string_length(v, JSON_C_TO_STRING_PLAIN, &bytes);
    return charge(bytes, remaining, e, n);
}
static int render_string(const char *s, json_object *ctx, json_object **out, bool optional,
                         size_t *remaining, char *e, size_t n)
{
    if (template_syntax(s, e, n))
        return -1;
    const char *a = strstr(s, "{{");
    if (!a) {
        *out = json_object_new_string(s);
        if (charge_value(*out, remaining, e, n)) {
            json_object_put(*out);
            *out = NULL;
            return -1;
        }
        return 0;
    }
    const char *b = strstr(a + 2, "}}");
    if (a == s && b[2] == 0) {
        char *path = trim_copy(a + 2, (size_t)(b - a - 2));
        bool found = false;
        json_object *v = lookup(ctx, path, &found);
        if (!found && !optional) {
            int rc = fail(e, n, "Missing variable: %s", path);
            free(path);
            return rc;
        }
        free(path);
        if (charge_value(v, remaining, e, n))
            return -1;
        *out = flow_clone(v);
        return 0;
    }
    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    if (!f)
        return fail(e, n, "Out of memory");
    const char *p = s;
    int rc = 0;
    while ((a = strstr(p, "{{"))) {
        if ((size_t)ftell(f) + (size_t)(a - p) > *remaining) {
            rc = fail(e, n, "Template output exceeds 4 MiB");
            break;
        }
        fwrite(p, 1, (size_t)(a - p), f);
        b = strstr(a + 2, "}}");
        char *path = trim_copy(a + 2, (size_t)(b - a - 2));
        bool found = false;
        json_object *v = lookup(ctx, path, &found);
        if (!v || json_object_is_type(v, json_type_array) ||
            json_object_is_type(v, json_type_object)) {
            rc = fail(e, n, "Missing or non-scalar variable: %s", path);
            free(path);
            break;
        }
        free(path);
        if ((size_t)ftell(f) + strlen(json_object_get_string(v)) > *remaining) {
            rc = fail(e, n, "Template output exceeds 4 MiB");
            break;
        }
        fputs(json_object_get_string(v), f);
        p = b + 2;
        if (ftell(f) > 4194304) {
            rc = fail(e, n, "Template output exceeds 4 MiB");
            break;
        }
    }
    if (!rc && (size_t)ftell(f) + strlen(p) > *remaining)
        rc = fail(e, n, "Template output exceeds 4 MiB");
    if (!rc)
        fputs(p, f);
    if (fclose(f))
        rc = fail(e, n, "Template allocation failed");
    if (!rc) {
        *out = json_object_new_string(buf);
        if (charge_value(*out, remaining, e, n)) {
            json_object_put(*out);
            *out = NULL;
            rc = -1;
        }
    }
    free(buf);
    return rc;
}
static int render_value(json_object *v, json_object *ctx, json_object **out, int depth,
                        size_t *remaining, char *e, size_t n)
{
    *out = NULL;
    if (depth > 32)
        return fail(e, n, "Template nesting exceeds 32");
    if (json_object_is_type(v, json_type_string))
        return render_string(json_object_get_string(v), ctx, out, false, remaining, e, n);
    if (json_object_is_type(v, json_type_object)) {
        if (charge(2, remaining, e, n))
            return -1;
        *out = json_object_new_object();
        json_object_object_foreach(v, k, child)
        {
            json_object *rendered = NULL;
            if (charge(strlen(k) * 6 + 4, remaining, e, n) ||
                render_value(child, ctx, &rendered, depth + 1, remaining, e, n)) {
                json_object_put(*out);
                *out = NULL;
                return -1;
            }
            json_object_object_add(*out, k, rendered);
        }
        return 0;
    }
    if (json_object_is_type(v, json_type_array)) {
        if (charge(2, remaining, e, n))
            return -1;
        *out = json_object_new_array();
        for (size_t i = 0; i < json_object_array_length(v); i++) {
            json_object *rendered = NULL;
            if (charge(1, remaining, e, n) || render_value(json_object_array_get_idx(v, i), ctx,
                                                           &rendered, depth + 1, remaining, e, n)) {
                json_object_put(*out);
                *out = NULL;
                return -1;
            }
            json_object_array_add(*out, rendered);
        }
        return 0;
    }
    if (charge_value(v, remaining, e, n))
        return -1;
    *out = flow_clone(v);
    return 0;
}
int flow_render(json_object *v, json_object *ctx, json_object **out, char *e, size_t n)
{
    size_t remaining = 4194304;
    return render_value(v, ctx, out, 0, &remaining, e, n);
}
static bool numeric(json_object *v)
{
    return json_object_is_type(v, json_type_int) || json_object_is_type(v, json_type_double);
}
int flow_test(json_object *test, json_object *ctx, bool *result, char *e, size_t n)
{
    json_object *left = NULL, *right = NULL;
    const char *op = js(test, "op", "exists");
    json_object *expr = jg(test, "left");
    int rc;
    size_t remaining = 4194304;
    if (!strcmp(op, "exists") && json_object_is_type(expr, json_type_string))
        rc = render_string(json_object_get_string(expr), ctx, &left, true, &remaining, e, n);
    else
        rc = flow_render(expr, ctx, &left, e, n);
    if (rc)
        return -1;
    if (!strcmp(op, "exists")) {
        *result = left && (!json_object_is_type(left, json_type_string) ||
                           json_object_get_string_len(left) > 0);
        json_object_put(left);
        return 0;
    }
    if (flow_render(jg(test, "right"), ctx, &right, e, n)) {
        json_object_put(left);
        return -1;
    }
    bool equal = numeric(left) && numeric(right)
                     ? json_object_get_double(left) == json_object_get_double(right)
                     : json_object_equal(left, right);
    if (!strcmp(op, "eq"))
        *result = equal;
    else if (!strcmp(op, "ne"))
        *result = !equal;
    else if (!strcmp(op, "contains")) {
        *result = false;
        if (json_object_is_type(left, json_type_string) &&
            json_object_is_type(right, json_type_string))
            *result = strstr(json_object_get_string(left), json_object_get_string(right)) != NULL;
        else if (json_object_is_type(left, json_type_array))
            for (size_t i = 0; i < json_object_array_length(left); i++)
                if (json_object_equal(json_object_array_get_idx(left, i), right))
                    *result = true;
    } else if (numeric(left) && numeric(right)) {
        double a = json_object_get_double(left), b = json_object_get_double(right);
        *result = !strcmp(op, "gt")    ? a > b
                  : !strcmp(op, "gte") ? a >= b
                  : !strcmp(op, "lt")  ? a < b
                                       : a <= b;
    } else
        rc = fail(e, n, "Ordered comparison requires numeric operands");
    json_object_put(left);
    json_object_put(right);
    return rc;
}
