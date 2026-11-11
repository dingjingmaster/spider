#include "rule/rule.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DATA_BYTES (4U * 1024U * 1024U)
#define DATA_ARRAY 1000U
#define DATA_DEPTH 32

static bool has(json_object *o, const char *key, json_object **out)
{
    json_object *ignored = NULL;
    return o && json_object_is_type(o, json_type_object) &&
           json_object_object_get_ex(o, key, out ? out : &ignored);
}
static void clear(char *e, size_t n)
{
    if (e && n)
        *e = 0;
}
static int error(char *e, size_t n, const char *path, const char *reason)
{
    return fail(e, n, "%s: %s", *path ? path : "/", reason);
}
static int expression_error(char *e, size_t n, const char *path)
{
    char cause[SP_ERROR];
    snprintf(cause, sizeof(cause), "%s", e && n && *e ? e : "expression failed");
    return error(e, n, path, cause);
}
static char *path_key(const char *path, const char *key)
{
    size_t length = strlen(path), extra = strlen(key);
    char *out = malloc(length + extra * 2 + 2);
    if (!out)
        return NULL;
    memcpy(out, path, length);
    out[length++] = '/';
    for (; *key; key++) {
        if (*key == '~' || *key == '/') {
            out[length++] = '~';
            out[length++] = *key == '~' ? '0' : '1';
        } else
            out[length++] = *key;
    }
    out[length] = 0;
    return out;
}
static int shape(json_object *v, int depth, const char *path, bool limit_arrays, char *e, size_t n)
{
    if (depth > DATA_DEPTH)
        return error(e, n, path, "nesting exceeds 32");
    if (json_object_is_type(v, json_type_double) && !isfinite(json_object_get_double(v)))
        return error(e, n, path, "number must be finite");
    if (json_object_is_type(v, json_type_array)) {
        if (limit_arrays && json_object_array_length(v) > DATA_ARRAY)
            return error(e, n, path, "array exceeds 1000 elements");
        for (size_t i = 0; i < json_object_array_length(v); i++) {
            char index[32];
            snprintf(index, sizeof(index), "%zu", i);
            char *child = path_key(path, index);
            if (!child)
                return error(e, n, path, "allocation failed");
            int rc = shape(json_object_array_get_idx(v, i), depth + 1, child, limit_arrays, e, n);
            free(child);
            if (rc)
                return -1;
        }
    } else if (json_object_is_type(v, json_type_object)) {
        json_object_object_foreach(v, key, value)
        {
            char *child = path_key(path, key);
            if (!child)
                return error(e, n, path, "allocation failed");
            int rc = shape(value, depth + 1, child, limit_arrays, e, n);
            free(child);
            if (rc)
                return -1;
        }
    }
    return 0;
}
static size_t bytes(json_object *v)
{
    size_t length = 0;
    json_object_to_json_string_length(v, JSON_C_TO_STRING_PLAIN, &length);
    return length;
}
static int bounded_arrays(json_object *v, const char *path, bool limit_arrays, char *e, size_t n)
{
    if (shape(v, 0, path, limit_arrays, e, n))
        return -1;
    if (bytes(v) > DATA_BYTES)
        return error(e, n, path, "value exceeds 4 MiB");
    return 0;
}
static int bounded(json_object *v, const char *path, char *e, size_t n)
{
    return bounded_arrays(v, path, true, e, n);
}
/* Context contains unrelated HTTP history, which may hold up to 10000 items.
 * The transform cap applies to selected data, not all retained branch values. */
static int context_bounded(json_object *v, char *e, size_t n)
{
    return bounded_arrays(v, "", false, e, n);
}
static bool type_name(const char *type)
{
    return !strcmp(type, "string") || !strcmp(type, "number") || !strcmp(type, "integer") ||
           !strcmp(type, "boolean") || !strcmp(type, "object") || !strcmp(type, "array") ||
           !strcmp(type, "null");
}
static bool matches(json_object *v, const char *type)
{
    enum json_type t = json_object_get_type(v);
    return (!strcmp(type, "null") && t == json_type_null) ||
           (!strcmp(type, "string") && t == json_type_string) ||
           (!strcmp(type, "number") && (t == json_type_int || t == json_type_double)) ||
           (!strcmp(type, "integer") && t == json_type_int) ||
           (!strcmp(type, "boolean") && t == json_type_boolean) ||
           (!strcmp(type, "object") && t == json_type_object) ||
           (!strcmp(type, "array") && t == json_type_array);
}
static int apply_schema(json_object *, json_object **, bool *, const char *, int, size_t *, char *,
                        size_t);
static int schema(json_object *s, const char *path, int depth, char *e, size_t n)
{
    if (depth > DATA_DEPTH)
        return error(e, n, path, "schema nesting exceeds 32");
    if (!s || !json_object_is_type(s, json_type_object))
        return error(e, n, path, "schema must be an object");
    json_object_object_foreach(s, keyword, ignored)
    {
        (void)ignored;
        if (strcmp(keyword, "type") && strcmp(keyword, "properties") &&
            strcmp(keyword, "required") && strcmp(keyword, "items") && strcmp(keyword, "default") &&
            strcmp(keyword, "additionalProperties"))
            return error(e, n, path, "unsupported schema keyword");
    }
    json_object *v = NULL;
    if (has(s, "type", &v)) {
        if (json_object_is_type(v, json_type_string)) {
            if (!type_name(json_object_get_string(v)) ||
                strlen(json_object_get_string(v)) != (size_t)json_object_get_string_len(v))
                return error(e, n, path, "unsupported schema type");
        } else if (json_object_is_type(v, json_type_array) && json_object_array_length(v)) {
            for (size_t i = 0; i < json_object_array_length(v); i++) {
                json_object *t = json_object_array_get_idx(v, i);
                if (!json_object_is_type(t, json_type_string) ||
                    strlen(json_object_get_string(t)) != (size_t)json_object_get_string_len(t) ||
                    !type_name(json_object_get_string(t)))
                    return error(e, n, path, "invalid schema type array");
                for (size_t j = 0; j < i; j++)
                    if (json_object_equal(t, json_object_array_get_idx(v, j)))
                        return error(e, n, path, "duplicate schema type");
            }
        } else
            return error(e, n, path, "type must be a name or nonempty array");
    }
    if (has(s, "properties", &v)) {
        if (!v || !json_object_is_type(v, json_type_object))
            return error(e, n, path, "properties must be an object");
        json_object_object_foreach(v, key, child_schema)
        {
            char *child = path_key(path, key);
            if (!child)
                return error(e, n, path, "allocation failed");
            int rc = schema(child_schema, child, depth + 1, e, n);
            free(child);
            if (rc)
                return -1;
        }
    }
    if (has(s, "required", &v)) {
        if (!v || !json_object_is_type(v, json_type_array))
            return error(e, n, path, "required must be an array");
        for (size_t i = 0; i < json_object_array_length(v); i++) {
            json_object *key = json_object_array_get_idx(v, i);
            if (!json_object_is_type(key, json_type_string) ||
                strlen(json_object_get_string(key)) != (size_t)json_object_get_string_len(key))
                return error(e, n, path, "required entries must be strings");
            for (size_t j = 0; j < i; j++)
                if (json_object_equal(key, json_object_array_get_idx(v, j)))
                    return error(e, n, path, "duplicate required property");
        }
    }
    if (has(s, "items", &v) && schema(v, path, depth + 1, e, n))
        return -1;
    if (has(s, "additionalProperties", &v) && !json_object_is_type(v, json_type_boolean))
        return error(e, n, path, "additionalProperties must be boolean");
    if (has(s, "default", &v)) {
        json_object *copy = flow_clone(v);
        bool present = true;
        size_t used = bytes(copy);
        int rc = apply_schema(s, &copy, &present, path, depth, &used, e, n);
        json_object_put(copy);
        if (rc)
            return -1;
    }
    return 0;
}
int flow_schema_validate(json_object *s, char *e, size_t n)
{
    clear(e, n);
    return bounded(s, "/input_schema", e, n) || schema(s, "/input_schema", 0, e, n) ? -1 : 0;
}
static int apply_schema(json_object *s, json_object **v, bool *present, const char *path, int depth,
                        size_t *used, char *e, size_t n)
{
    if (depth > DATA_DEPTH)
        return error(e, n, path, "value nesting exceeds 32");
    if (!*present) {
        json_object *def = NULL;
        if (!has(s, "default", &def))
            return 0;
        size_t add = bytes(def);
        if (*used > DATA_BYTES || add > DATA_BYTES - *used)
            return error(e, n, path, "expanded defaults exceed 4 MiB");
        *used += add;
        *v = flow_clone(def);
        *present = true;
    }
    json_object *types = jg(s, "type");
    bool valid = !types;
    if (json_object_is_type(types, json_type_string))
        valid = matches(*v, json_object_get_string(types));
    else if (json_object_is_type(types, json_type_array))
        for (size_t i = 0; i < json_object_array_length(types); i++)
            valid =
                valid || matches(*v, json_object_get_string(json_object_array_get_idx(types, i)));
    if (!valid)
        return error(e, n, path, "type does not match schema");
    if (json_object_is_type(*v, json_type_object)) {
        json_object *properties = jg(s, "properties");
        if (properties) {
            json_object_object_foreach(properties, key, child_schema)
            {
                json_object *child_value = NULL;
                bool existed = has(*v, key, &child_value), exists = existed;
                char *child = path_key(path, key);
                if (!child)
                    return error(e, n, path, "allocation failed");
                if (!existed && has(child_schema, "default", NULL)) {
                    json_object *encoded_key = json_object_new_string(key);
                    size_t add = bytes(encoded_key) + 1 + (json_object_object_length(*v) ? 1 : 0);
                    json_object_put(encoded_key);
                    if (*used > DATA_BYTES || add > DATA_BYTES - *used) {
                        free(child);
                        return error(e, n, path, "expanded defaults exceed 4 MiB");
                    }
                    *used += add;
                }
                int rc =
                    apply_schema(child_schema, &child_value, &exists, child, depth + 1, used, e, n);
                free(child);
                if (rc) {
                    if (!existed)
                        json_object_put(child_value);
                    return -1;
                }
                if (!existed && exists)
                    json_object_object_add(*v, key, child_value);
            }
        }
        json_object *required = jg(s, "required");
        for (size_t i = 0; required && i < json_object_array_length(required); i++) {
            const char *key = json_object_get_string(json_object_array_get_idx(required, i));
            if (!has(*v, key, NULL)) {
                char *child = path_key(path, key);
                int rc = error(e, n, child ? child : path, "required property is missing");
                free(child);
                return rc;
            }
        }
        json_object *additional = NULL;
        if (has(s, "additionalProperties", &additional) && !json_object_get_boolean(additional)) {
            json_object_object_foreach(*v, key, ignored)
            {
                (void)ignored;
                if (!has(properties, key, NULL)) {
                    char *child = path_key(path, key);
                    int rc = error(e, n, child ? child : path, "additional property is forbidden");
                    free(child);
                    return rc;
                }
            }
        }
    } else if (json_object_is_type(*v, json_type_array) && jg(s, "items")) {
        for (size_t i = 0; i < json_object_array_length(*v); i++) {
            char index[32];
            snprintf(index, sizeof(index), "%zu", i);
            char *child = path_key(path, index);
            if (!child)
                return error(e, n, path, "allocation failed");
            json_object *item = json_object_array_get_idx(*v, i);
            bool exists = true;
            int rc = apply_schema(jg(s, "items"), &item, &exists, child, depth + 1, used, e, n);
            free(child);
            if (rc)
                return -1;
        }
    }
    return 0;
}
int flow_input_prepare(json_object *step, json_object *ctx, char *e, size_t n)
{
    clear(e, n);
    if (!ctx || !json_object_is_type(ctx, json_type_object))
        return error(e, n, "/input", "context must be an object");
    json_object *mapping = NULL, *s = NULL, *value = NULL;
    bool mapped = has(step, "input", &mapping), contracted = has(step, "input_schema", &s);
    if (!mapped && !contracted)
        return 0;
    if (context_bounded(ctx, e, n) || (contracted && flow_schema_validate(s, e, n)))
        return -1;
    bool present;
    if (mapped) {
        if (flow_render(mapping, ctx, &value, e, n))
            return expression_error(e, n, "/input");
        present = true;
    } else {
        json_object *old = NULL;
        present = has(ctx, "input", &old);
        value = flow_clone(old);
    }
    size_t used = present ? bytes(value) : 0;
    if (contracted && apply_schema(s, &value, &present, "/input", 0, &used, e, n)) {
        json_object_put(value);
        return -1;
    }
    if (!present) {
        json_object_put(value);
        return error(e, n, "/input", "input is missing");
    }
    if (bounded(value, "/input", e, n)) {
        json_object_put(value);
        return -1;
    }
    json_object *candidate = flow_clone(ctx);
    if (!candidate) {
        json_object_put(value);
        return error(e, n, "/input", "allocation failed");
    }
    json_object_object_add(candidate, "input", json_object_get(value));
    int rc = context_bounded(candidate, e, n);
    json_object_put(candidate);
    if (rc)
        json_object_put(value);
    else
        json_object_object_add(ctx, "input", value);
    return rc;
}
static bool pointer_valid(const char *path)
{
    if (*path && *path != '/')
        return false;
    for (; *path; path++)
        if (*path == '~' && (*++path != '0' && *path != '1'))
            return false;
    return true;
}
static bool string_field(json_object *o, const char *key)
{
    json_object *v = NULL;
    return has(o, key, &v) && json_object_is_type(v, json_type_string) &&
           strlen(json_object_get_string(v)) == (size_t)json_object_get_string_len(v);
}
int flow_transform_validate(json_object *step, char *e, size_t n)
{
    clear(e, n);
    if (bounded(step, "", e, n))
        return -1;
    json_object *operations = jg(step, "operations");
    if (!operations || !json_object_is_type(operations, json_type_array) ||
        !json_object_array_length(operations) || json_object_array_length(operations) > 32)
        return error(e, n, "/operations", "operations must contain 1-32 entries");
    for (size_t i = 0; i < json_object_array_length(operations); i++) {
        json_object *op = json_object_array_get_idx(operations, i);
        const char *name = js(op, "op", "");
        char path[64];
        snprintf(path, sizeof(path), "/operations/%zu", i);
        bool valid = false;
        if (!strcmp(name, "pick"))
            valid = string_field(op, "path") && pointer_valid(js(op, "path", ""));
        else if (!strcmp(name, "default"))
            valid = has(op, "value", NULL);
        else if (!strcmp(name, "rename"))
            valid = string_field(op, "from") && string_field(op, "to");
        else if (!strcmp(name, "cast")) {
            const char *type = js(op, "type", "");
            valid = string_field(op, "type") && type_name(type) && strcmp(type, "object") &&
                    strcmp(type, "array");
        } else if (!strcmp(name, "trim"))
            valid = true;
        else if (!strcmp(name, "replace"))
            valid = string_field(op, "old") && *js(op, "old", "") && string_field(op, "new");
        else if (!strcmp(name, "map"))
            valid = json_object_is_type(jg(op, "fields"), json_type_object);
        else if (!strcmp(name, "filter")) {
            json_object *test = jg(op, "test");
            const char *cmp = js(test, "op", "exists");
            valid = test && json_object_is_type(test, json_type_object) &&
                    (!has(test, "op", NULL) || string_field(test, "op")) &&
                    has(test, "left", NULL) &&
                    (!strcmp(cmp, "exists") ||
                     ((!strcmp(cmp, "eq") || !strcmp(cmp, "ne") || !strcmp(cmp, "gt") ||
                       !strcmp(cmp, "gte") || !strcmp(cmp, "lt") || !strcmp(cmp, "lte") ||
                       !strcmp(cmp, "contains")) &&
                      has(test, "right", NULL)));
        }
        if (!valid || !string_field(op, "op"))
            return error(e, n, path, "invalid transform operation");
    }
    return 0;
}
/* JSON numeric grammar, deliberately excluding whitespace, plus signs, NaN,
 * infinity and partially parsed strings accepted by strtod. */
static bool number_string(const char *s, bool integer)
{
    if (*s == '-')
        s++;
    if (*s == '0')
        s++;
    else {
        if (*s < '1' || *s > '9')
            return false;
        while (*s >= '0' && *s <= '9')
            s++;
    }
    if (!integer && *s == '.') {
        s++;
        if (*s < '0' || *s > '9')
            return false;
        while (*s >= '0' && *s <= '9')
            s++;
    }
    if (!integer && (*s == 'e' || *s == 'E')) {
        s++;
        if (*s == '+' || *s == '-')
            s++;
        if (*s < '0' || *s > '9')
            return false;
        while (*s >= '0' && *s <= '9')
            s++;
    }
    return !*s;
}
static int cast(json_object *v, const char *type, json_object **out, const char *path, char *e,
                size_t n)
{
    *out = NULL;
    if (!v) {
        if (!strcmp(type, "null"))
            return 0;
        return error(e, n, path, "cannot cast null without an explicit default");
    }
    if (json_object_is_type(v, json_type_object) || json_object_is_type(v, json_type_array))
        return error(e, n, path, "cast requires a scalar");
    bool text = json_object_is_type(v, json_type_string);
    const char *s = json_object_get_string(v);
    if (text && strlen(s) != (size_t)json_object_get_string_len(v))
        return error(e, n, path, "cast string contains an embedded null");
    if (!strcmp(type, "string")) {
        *out =
            text
                ? flow_clone(v)
                : json_object_new_string(json_object_to_json_string_ext(v, JSON_C_TO_STRING_PLAIN));
        return 0;
    }
    if (!strcmp(type, "boolean")) {
        if (json_object_is_type(v, json_type_boolean))
            *out = flow_clone(v);
        else if (text && (!strcmp(s, "true") || !strcmp(s, "false")))
            *out = json_object_new_boolean(!strcmp(s, "true"));
        else
            return error(e, n, path, "invalid boolean cast");
        return 0;
    }
    if (!strcmp(type, "integer")) {
        int64_t value;
        if (json_object_is_type(v, json_type_int)) {
            if (*s != '-' && json_object_get_uint64(v) > (uint64_t)INT64_MAX)
                return error(e, n, path, "integer cast is out of range");
            value = json_object_get_int64(v);
        } else if (json_object_is_type(v, json_type_double)) {
            double d = json_object_get_double(v);
            if (!isfinite(d) || trunc(d) != d || (long double)d < -9223372036854775808.0L ||
                (long double)d >= 9223372036854775808.0L)
                return error(e, n, path, "integer cast is fractional or out of range");
            value = (int64_t)d;
        } else if (text && number_string(s, true)) {
            errno = 0;
            char *end = NULL;
            long long parsed = strtoll(s, &end, 10);
            if (errno == ERANGE || !end || *end)
                return error(e, n, path, "integer cast is out of range");
            value = (int64_t)parsed;
        } else
            return error(e, n, path, "invalid integer cast");
        *out = json_object_new_int64(value);
        return 0;
    }
    if (!strcmp(type, "number")) {
        double d;
        if (json_object_is_type(v, json_type_int) || json_object_is_type(v, json_type_double))
            d = json_object_get_double(v);
        else if (text && number_string(s, false)) {
            errno = 0;
            char *end = NULL;
            d = strtod(s, &end);
            if (errno == ERANGE || !end || *end)
                return error(e, n, path, "number cast is out of range");
        } else
            return error(e, n, path, "invalid number cast");
        if (!isfinite(d))
            return error(e, n, path, "number cast must be finite");
        *out = json_object_new_double(d);
        return 0;
    }
    return error(e, n, path, "invalid null cast");
}
/* Repeated-prefix patterns must not turn a bounded value into quadratic work.
 * KMP also preserves embedded NUL bytes in source values. */
static const char *find_match(const char *source, size_t size, size_t offset, const char *pattern,
                              size_t length, const uint32_t *prefix)
{
    if (length == 1)
        return memchr(source + offset, (unsigned char)*pattern, size - offset);
    size_t matched = 0;
    for (size_t i = offset; i < size; i++) {
        while (matched && source[i] != pattern[matched])
            matched = prefix[matched - 1];
        if (source[i] == pattern[matched])
            matched++;
        if (matched == length)
            return source + i + 1 - length;
    }
    return NULL;
}
static int replace(json_object *value, json_object *op, json_object **out, const char *path,
                   char *e, size_t n)
{
    if (!json_object_is_type(value, json_type_string))
        return error(e, n, path, "replace requires a string");
    const char *source = json_object_get_string(value), *old = js(op, "old", ""),
               *replacement = js(op, "new", "");
    size_t size = (size_t)json_object_get_string_len(value), old_size = strlen(old),
           new_size = strlen(replacement), count = 0;
    uint32_t *prefix = old_size > 1 ? calloc(old_size, sizeof(*prefix)) : NULL;
    if (old_size > 1 && !prefix)
        return error(e, n, path, "allocation failed");
    for (size_t i = 1, matched = 0; i < old_size; i++) {
        while (matched && old[i] != old[matched])
            matched = prefix[matched - 1];
        if (old[i] == old[matched])
            matched++;
        prefix[i] = (uint32_t)matched;
    }
    size_t offset = 0;
    const char *match;
    while ((match = find_match(source, size, offset, old, old_size, prefix))) {
        count++;
        offset = (size_t)(match - source) + old_size;
    }
    size_t result_size = size - count * old_size;
    if (new_size && count > (DATA_BYTES - result_size) / new_size) {
        free(prefix);
        return error(e, n, path, "replace output exceeds 4 MiB");
    }
    result_size += count * new_size;
    char *buffer = malloc(result_size + 1);
    if (!buffer) {
        free(prefix);
        return error(e, n, path, "allocation failed");
    }
    size_t at = 0;
    offset = 0;
    while ((match = find_match(source, size, offset, old, old_size, prefix))) {
        size_t literal_size = (size_t)(match - source) - offset;
        memcpy(buffer + at, source + offset, literal_size);
        at += literal_size;
        memcpy(buffer + at, replacement, new_size);
        at += new_size;
        offset = (size_t)(match - source) + old_size;
    }
    memcpy(buffer + at, source + offset, size - offset);
    at += size - offset;
    buffer[at] = 0;
    *out = json_object_new_string_len(buffer, (int)at);
    free(buffer);
    free(prefix);
    return 0;
}
static int array_op(json_object *value, json_object *op, json_object *ctx, json_object **out,
                    const char *path, char *e, size_t n)
{
    if (!json_object_is_type(value, json_type_array))
        return error(e, n, path, "operation requires an array");
    json_object *local = flow_clone(ctx), *result = json_object_new_array();
    if (!local) {
        json_object_put(result);
        return error(e, n, path, "allocation failed");
    }
    size_t total = 2;
    int rc = 0;
    for (size_t i = 0; i < json_object_array_length(value); i++) {
        json_object *item = json_object_array_get_idx(value, i), *mapped = NULL;
        json_object_object_add(local, "input", flow_clone(item));
        bool include = true;
        if (!strcmp(js(op, "op", ""), "filter")) {
            rc = flow_test(jg(op, "test"), local, &include, e, n);
            if (!rc && include)
                mapped = flow_clone(item);
        } else
            rc = flow_render(jg(op, "fields"), local, &mapped, e, n);
        if (rc) {
            rc = expression_error(e, n, path);
            json_object_put(mapped);
            break;
        }
        if (include) {
            total += bytes(mapped) + 1;
            if (total > DATA_BYTES) {
                json_object_put(mapped);
                rc = error(e, n, path, "array output exceeds 4 MiB");
                break;
            }
            json_object_array_add(result, mapped);
        }
    }
    json_object_put(local);
    if (rc)
        json_object_put(result);
    else
        *out = result;
    return rc;
}
int flow_transform(json_object *step, json_object *ctx, json_object **out, char *e, size_t n)
{
    *out = NULL;
    clear(e, n);
    if (!ctx || !json_object_is_type(ctx, json_type_object))
        return error(e, n, "/input", "context must be an object");
    if (flow_transform_validate(step, e, n) || context_bounded(ctx, e, n))
        return -1;
    json_object *value = NULL, *expression = NULL;
    if (has(step, "value", &expression)) {
        if (flow_render(expression, ctx, &value, e, n))
            return expression_error(e, n, "/value");
    } else {
        if (!has(ctx, "input", &expression))
            return error(e, n, "/input", "input is missing");
        value = flow_clone(expression);
    }
    if (bounded(value, "/value", e, n)) {
        json_object_put(value);
        return -1;
    }
    json_object *ops = jg(step, "operations");
    int rc = 0;
    for (size_t i = 0; i < json_object_array_length(ops); i++) {
        json_object *op = json_object_array_get_idx(ops, i), *next = NULL;
        const char *name = js(op, "op", "");
        char path[64];
        snprintf(path, sizeof(path), "/operations/%zu", i);
        if (!strcmp(name, "pick")) {
            json_object *picked = NULL;
            if (!json_pointer_lookup(value, js(op, "path", ""), &picked))
                next = flow_clone(picked);
            else if (has(op, "default", &picked))
                next = flow_clone(picked);
            else
                rc = error(e, n, path, "JSON Pointer path is missing");
        } else if (!strcmp(name, "default"))
            next = flow_clone(value ? value : jg(op, "value"));
        else if (!strcmp(name, "rename")) {
            json_object *child = NULL;
            const char *from = js(op, "from", ""), *to = js(op, "to", "");
            if (!json_object_is_type(value, json_type_object))
                rc = error(e, n, path, "rename requires an object");
            else if (!has(value, from, &child))
                rc = error(e, n, path, "rename source is missing");
            else if (strcmp(from, to) && has(value, to, NULL))
                rc = error(e, n, path, "rename target already exists");
            else {
                next = flow_clone(value);
                if (strcmp(from, to)) {
                    json_object_object_add(next, to, flow_clone(child));
                    json_object_object_del(next, from);
                }
            }
        } else if (!strcmp(name, "cast"))
            rc = cast(value, js(op, "type", ""), &next, path, e, n);
        else if (!strcmp(name, "trim")) {
            if (!json_object_is_type(value, json_type_string))
                rc = error(e, n, path, "trim requires a string");
            else {
                const char *s = json_object_get_string(value);
                size_t a = 0, b = (size_t)json_object_get_string_len(value);
                while (a < b && isspace((unsigned char)s[a]))
                    a++;
                while (b > a && isspace((unsigned char)s[b - 1]))
                    b--;
                next = json_object_new_string_len(s + a, (int)(b - a));
            }
        } else if (!strcmp(name, "replace"))
            rc = replace(value, op, &next, path, e, n);
        else
            rc = array_op(value, op, ctx, &next, path, e, n);
        json_object_put(value);
        value = next;
        if (rc || bounded(value, path, e, n)) {
            rc = -1;
            break;
        }
    }
    if (rc)
        json_object_put(value);
    else
        *out = value;
    return rc;
}
