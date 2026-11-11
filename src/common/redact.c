#include "redact.h"
#include "rule/rule.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define SECRET_COUNT 512U
#define SECRET_BYTES (1024U * 1024U)
#define PRESENTATION_BYTES (4U * 1024U * 1024U)

typedef struct {
    json_object *values;
    size_t bytes;
    bool failed;
} Secrets;
static bool same_name(const char *a, const char *b)
{
    while (*a || *b) {
        while (*a == '-' || *a == '_')
            a++;
        while (*b == '-' || *b == '_')
            b++;
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return false;
        if (!*a)
            return true;
        a++;
        b++;
    }
    return true;
}
static bool suffix(const char *key, const char *ending)
{
    size_t a = strlen(key), b = strlen(ending);
    while (a && b) {
        if (key[a - 1] == '-' || key[a - 1] == '_') {
            a--;
            continue;
        }
        if (tolower((unsigned char)key[--a]) != (unsigned char)ending[--b])
            return false;
    }
    return !b;
}
static bool sensitive(const char *key, json_object *rule)
{
    const char *names[] = {"passwd",        "authorization", "cookie",
                           "set-cookie",    "access_key",    "private_key",
                           "credentials",   "credential",    "proxy-authorization",
                           "header_values", "_debug_secrets"};
    const char *endings[] = {"token", "password", "secret", "apikey"};
    for (size_t i = 0; i < sizeof(names) / sizeof(*names); i++)
        if (same_name(key, names[i]))
            return true;
    for (size_t i = 0; i < sizeof(endings) / sizeof(*endings); i++)
        if (suffix(key, endings[i]))
            return true;
    json_object *custom = jg(rule, "sensitive_fields");
    if (custom && json_object_is_type(custom, json_type_array))
        for (size_t i = 0; i < json_object_array_length(custom); i++) {
            json_object *v = json_object_array_get_idx(custom, i);
            if (json_object_is_type(v, json_type_string) &&
                same_name(key, json_object_get_string(v)))
                return true;
        }
    return false;
}
static void add_secret(Secrets *s, const char *value, size_t len)
{
    if (s->failed || !len)
        return;
    for (size_t i = 0; i < json_object_array_length(s->values); i++) {
        json_object *known = json_object_array_get_idx(s->values, i);
        if (json_object_is_type(known, json_type_string) &&
            (size_t)json_object_get_string_len(known) == len &&
            !memcmp(json_object_get_string(known), value, len))
            return;
    }
    if (json_object_array_length(s->values) >= SECRET_COUNT || len > SECRET_BYTES - s->bytes) {
        s->failed = true;
        return;
    }
    json_object *v = json_object_new_string_len(value, (int)len);
    if (!v || json_object_array_add(s->values, v)) {
        json_object_put(v);
        s->failed = true;
        return;
    }
    s->bytes += len;
    /* A raw token may later be extracted from an Authorization value. */
    if (len > 7 && !strncasecmp(value, "Bearer ", 7))
        add_secret(s, value + 7, len - 7);
}
static void add_scalar(Secrets *s, json_object *value)
{
    if (s->failed || !value)
        return;
    for (size_t i = 0; i < json_object_array_length(s->values); i++)
        if (json_object_equal(value, json_object_array_get_idx(s->values, i)))
            return;
    size_t len = 0;
    const char *text = json_object_to_json_string_length(value, JSON_C_TO_STRING_PLAIN, &len);
    if (json_object_array_length(s->values) >= SECRET_COUNT || len > SECRET_BYTES - s->bytes) {
        s->failed = true;
        return;
    }
    json_object_array_add(s->values, flow_clone(value));
    s->bytes += len;
    /* Also protect string interpolation of a typed sensitive value. */
    add_secret(s, text, len);
}
static int hex(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    c = (char)tolower((unsigned char)c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}
static char *query_decode(const char *s, size_t n, size_t *len)
{
    char *out = malloc(n + 1);
    if (!out)
        return NULL;
    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '%' && i + 2 < n && hex(s[i + 1]) >= 0 && hex(s[i + 2]) >= 0) {
            out[j++] = (char)(hex(s[i + 1]) * 16 + hex(s[i + 2]));
            i += 2;
        } else
            out[j++] = s[i] == '+' ? ' ' : s[i];
    }
    out[j] = 0;
    *len = j;
    return out;
}
static bool query_sensitive(const char *s, size_t n, json_object *rule)
{
    size_t len = 0;
    char *name = query_decode(s, n, &len);
    if (!name)
        return true;
    bool result = sensitive(name, rule);
    free(name);
    return result;
}
static void collect_url(Secrets *s, const char *url, json_object *rule)
{
    const char *p = strchr(url, '?');
    if (!p || (strncmp(url, "http://", 7) && strncmp(url, "https://", 8)))
        return;
    p++;
    while (*p && *p != '#') {
        size_t part = strcspn(p, "&#");
        const char *eq = memchr(p, '=', part);
        if (eq && query_sensitive(p, (size_t)(eq - p), rule)) {
            size_t encoded = part - (size_t)(eq - p) - 1, len = 0;
            char *decoded = query_decode(eq + 1, encoded, &len);
            if (!decoded) {
                s->failed = true;
                return;
            }
            add_secret(s, decoded, len);
            add_secret(s, eq + 1, encoded);
            free(decoded);
        }
        p += part;
        if (*p == '&')
            p++;
    }
}
static void collect(Secrets *s, json_object *v, json_object *rule, bool marked, unsigned depth)
{
    if (!v || s->failed)
        return;
    if (depth > 64) {
        s->failed = true;
        return;
    }
    if (json_object_is_type(v, json_type_object)) {
        json_object_object_foreach(v, key, child)
        {
            collect(s, child, rule, marked || sensitive(key, rule), depth + 1);
        }
    } else if (json_object_is_type(v, json_type_array)) {
        for (size_t i = 0; i < json_object_array_length(v); i++)
            collect(s, json_object_array_get_idx(v, i), rule, marked, depth + 1);
    } else if (json_object_is_type(v, json_type_string)) {
        const char *value = json_object_get_string(v), *start = value;
        if (marked)
            add_secret(s, value, (size_t)json_object_get_string_len(v));
        while (isspace((unsigned char)*start))
            start++;
        if (*start == '{' || *start == '[') {
            json_object *parsed = NULL;
            if (!json_value_parse(value, &parsed, NULL, 0)) {
                collect(s, parsed, rule, marked, depth + 1);
                json_object_put(parsed);
            }
        }
        collect_url(s, value, rule);
    } else if (marked)
        add_scalar(s, v);
}
int debug_track_secrets(json_object *context, json_object *output, json_object *rule, char *e,
                        size_t n)
{
    if (!json_object_is_type(context, json_type_object))
        return fail(e, n, "Secret tracking requires an object context");
    Secrets s = {.values = json_object_new_array()};
    if (!s.values)
        return fail(e, n, "Cannot allocate debug secret tracking");
    collect(&s, rule, rule, false, 0);
    collect(&s, context, rule, false, 0);
    collect(&s, output, rule, false, 0);
    if (s.failed) {
        json_object_put(s.values);
        return fail(e, n, "Debug secret tracking exceeds 512 values or 1 MiB");
    }
    json_object_object_add(context, "_debug_secrets", s.values);
    return 0;
}
static bool contains_bytes(const char *text, size_t len, const char *value, size_t size)
{
    if (!size || size > len)
        return false;
    size_t available = len - size + 1;
    while (available) {
        const char *found = memchr(text, (unsigned char)*value, available);
        if (!found)
            return false;
        if (!memcmp(found, value, size))
            return true;
        available -= (size_t)(found - text) + 1;
        text = found + 1;
    }
    return false;
}
static bool tainted(json_object *value, json_object *rule, Secrets *known, unsigned depth)
{
    if (!value)
        return false;
    if (depth > 64)
        return true;
    if (json_object_is_type(value, json_type_object)) {
        json_object_object_foreach(value, key, child)
        {
            if (sensitive(key, rule) || tainted(child, rule, known, depth + 1))
                return true;
        }
    } else if (json_object_is_type(value, json_type_array)) {
        for (size_t i = 0; i < json_object_array_length(value); i++)
            if (tainted(json_object_array_get_idx(value, i), rule, known, depth + 1))
                return true;
    } else {
        for (size_t i = 0; i < json_object_array_length(known->values); i++) {
            json_object *secret = json_object_array_get_idx(known->values, i);
            if (json_object_equal(value, secret))
                return true;
            if (json_object_is_type(value, json_type_string) &&
                json_object_is_type(secret, json_type_string) &&
                contains_bytes(
                    json_object_get_string(value), (size_t)json_object_get_string_len(value),
                    json_object_get_string(secret), (size_t)json_object_get_string_len(secret)))
                return true;
        }
    }
    return false;
}
static bool rendered_tainted(json_object *expression, json_object *context, json_object *rule,
                             Secrets *known)
{
    json_object *value = NULL;
    /* Per-item input is not available here. A missing local reference must not
     * prevent inspecting independent run/steps references in the same map. */
    int rc = flow_render(expression, context, &value, NULL, 0);
    bool result = !rc && tainted(value, rule, known, 0);
    json_object_put(value);
    return result;
}
static bool operation_tainted(json_object *expression, json_object *context, json_object *rule,
                              Secrets *known, unsigned depth)
{
    if (!expression)
        return false;
    if (depth > 64 || tainted(expression, rule, known, 0))
        return true;
    if (json_object_is_type(expression, json_type_object)) {
        /* A JSON Pointer object is a single expression, not an ordinary map. */
        if (jg(expression, "$ref") && rendered_tainted(expression, context, rule, known))
            return true;
        json_object_object_foreach(expression, key, child)
        {
            (void)key;
            if (operation_tainted(child, context, rule, known, depth + 1))
                return true;
        }
    } else if (json_object_is_type(expression, json_type_array)) {
        for (size_t i = 0; i < json_object_array_length(expression); i++)
            if (operation_tainted(json_object_array_get_idx(expression, i), context, rule, known,
                                  depth + 1))
                return true;
    } else if (json_object_is_type(expression, json_type_string)) {
        const char *text = json_object_get_string(expression);
        if (!strstr(text, "{{"))
            return false;
        if (rendered_tainted(expression, context, rule, known))
            return true;
        /* Mixed templates can contain both an unbound item and a resolvable
         * secret. Resolve each reference separately after whole-string failure. */
        const char *start;
        while ((start = strstr(text, "{{"))) {
            const char *end = strstr(start + 2, "}}");
            if (!end)
                return true;
            json_object *part = json_object_new_string_len(start, (int)(end + 2 - start));
            if (!part)
                return true;
            bool result = rendered_tainted(part, context, rule, known);
            json_object_put(part);
            if (result)
                return true;
            text = end + 2;
        }
    }
    return false;
}
int debug_track_transform(json_object *context, json_object *step, json_object *output,
                          json_object *rule, char *e, size_t n)
{
    json_object *source = NULL, *expression = NULL;
    if (json_object_object_get_ex(step, "value", &expression)) {
        if (flow_render(expression, context, &source, e, n))
            return -1;
    } else
        source = flow_clone(jg(context, "input"));
    Secrets s = {.values = json_object_new_array()};
    if (!s.values) {
        json_object_put(source);
        return fail(e, n, "Cannot allocate transform secret tracking");
    }
    collect(&s, rule, rule, false, 0);
    collect(&s, context, rule, false, 0);
    /* Parsing JSON string sources can reveal sensitive keys as well. */
    size_t before = json_object_array_length(s.values);
    collect(&s, source, rule, false, 0);
    bool sensitive_source = before != json_object_array_length(s.values) ||
                            tainted(source, rule, &s, 0) ||
                            operation_tainted(jg(step, "operations"), context, rule, &s, 0);
    json_object_put(source);
    if (sensitive_source)
        collect(&s, output, rule, true, 0);
    if (s.failed) {
        json_object_put(s.values);
        return fail(e, n, "Debug secret tracking exceeds 512 values or 1 MiB");
    }
    json_object_object_add(context, "_debug_secrets", s.values);
    return 0;
}
static json_object *hide_values(json_object *v, Secrets *known)
{
    const char *value = json_object_get_string(v);
    size_t size = (size_t)json_object_get_string_len(v);
    /* Bucket candidates by first byte and scan the input once. Searching every
     * suffix again would be quadratic for common short sensitive values. */
    struct Candidate {
        const char *value;
        size_t size;
        int next;
    } entries[SECRET_COUNT];
    int heads[256];
    for (size_t i = 0; i < 256; i++)
        heads[i] = -1;
    for (size_t i = 0; i < json_object_array_length(known->values); i++) {
        json_object *secret = json_object_array_get_idx(known->values, i);
        if (!json_object_is_type(secret, json_type_string))
            continue;
        entries[i].value = json_object_get_string(secret);
        entries[i].size = (size_t)json_object_get_string_len(secret);
        unsigned char first = (unsigned char)entries[i].value[0];
        entries[i].next = heads[first];
        heads[first] = (int)i;
    }
    char *buffer = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buffer, &len);
    if (!f)
        return json_object_new_string("[REDACTED]");
    size_t start = 0, pos = 0, emitted = 0;
    bool too_large = false;
    while (pos < size) {
        size_t span = 0;
        for (int i = heads[(unsigned char)value[pos]]; i >= 0; i = entries[i].next)
            if (entries[i].size > span && entries[i].size <= size - pos &&
                !memcmp(value + pos, entries[i].value, entries[i].size))
                span = entries[i].size;
        if (!span) {
            pos++;
            continue;
        }
        size_t prefix = pos - start;
        if (prefix > PRESENTATION_BYTES - emitted || 10 > PRESENTATION_BYTES - emitted - prefix) {
            too_large = true;
            break;
        }
        fwrite(value + start, 1, prefix, f);
        fputs("[REDACTED]", f);
        emitted += prefix + 10;
        pos += span;
        start = pos;
    }
    if (!too_large && size - start <= PRESENTATION_BYTES - emitted)
        fwrite(value + start, 1, size - start, f);
    else
        too_large = true;
    int rc = fclose(f);
    json_object *out = rc || too_large ? json_object_new_string("[REDACTED]")
                                       : json_object_new_string_len(buffer, (int)len);
    free(buffer);
    return out;
}
static json_object *url_redact(const char *s, json_object *rule)
{
    const char *query = strchr(s, '?');
    if (!query || (strncmp(s, "http://", 7) && strncmp(s, "https://", 8)))
        return json_object_new_string(s);
    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    if (!f)
        return json_object_new_string("[REDACTED]");
    fwrite(s, 1, (size_t)(query - s + 1), f);
    const char *p = query + 1;
    while (*p && *p != '#') {
        size_t part = strcspn(p, "&#");
        const char *eq = memchr(p, '=', part);
        if (eq && query_sensitive(p, (size_t)(eq - p), rule)) {
            fwrite(p, 1, (size_t)(eq - p + 1), f);
            fputs("[REDACTED]", f);
        } else
            fwrite(p, 1, part, f);
        p += part;
        if (*p == '&')
            fputc(*p++, f);
    }
    fputs(p, f);
    int rc = fclose(f);
    json_object *out = json_object_new_string(rc ? "[REDACTED]" : buf);
    free(buf);
    return out;
}
static json_object *redact(json_object *v, json_object *rule, Secrets *known, unsigned depth)
{
    if (!v)
        return NULL;
    if (depth > 64)
        return json_object_new_string("[REDACTED]");
    if (json_object_is_type(v, json_type_object)) {
        json_object *out = json_object_new_object();
        json_object_object_foreach(v, key, child)
        {
            if (same_name(key, "_debug_secrets"))
                continue;
            json_object_object_add(out, key,
                                   sensitive(key, rule) ? json_object_new_string("[REDACTED]")
                                                        : redact(child, rule, known, depth + 1));
        }
        return out;
    }
    if (json_object_is_type(v, json_type_array)) {
        json_object *out = json_object_new_array();
        for (size_t i = 0; i < json_object_array_length(v); i++)
            json_object_array_add(out,
                                  redact(json_object_array_get_idx(v, i), rule, known, depth + 1));
        return out;
    }
    if (json_object_is_type(v, json_type_string)) {
        const char *s = json_object_get_string(v), *start = s;
        json_object *intermediate = NULL;
        while (isspace((unsigned char)*start))
            start++;
        if (*start == '{' || *start == '[') {
            json_object *parsed = NULL;
            if (!json_value_parse(s, &parsed, NULL, 0)) {
                json_object *clean = redact(parsed, rule, known, depth + 1);
                intermediate = json_object_new_string(
                    json_object_to_json_string_ext(clean, JSON_C_TO_STRING_PLAIN));
                json_object_put(clean);
                json_object_put(parsed);
            }
        }
        if (!intermediate)
            intermediate = strlen(s) == (size_t)json_object_get_string_len(v) ? url_redact(s, rule)
                                                                              : flow_clone(v);
        json_object *out = hide_values(intermediate, known);
        json_object_put(intermediate);
        return out;
    }
    for (size_t i = 0; i < json_object_array_length(known->values); i++)
        if (json_object_equal(v, json_object_array_get_idx(known->values, i)))
            return json_object_new_string("[REDACTED]");
    return flow_clone(v);
}
json_object *debug_redact_with_context(json_object *value, json_object *rule, json_object *source)
{
    Secrets s = {.values = json_object_new_array()};
    if (!s.values)
        return json_object_new_string("[REDACTED]");
    collect(&s, rule, rule, false, 0);
    collect(&s, source, rule, false, 0);
    if (source != value)
        collect(&s, value, rule, false, 0);
    json_object *out = s.failed ? json_object_new_string("[REDACTED]") : redact(value, rule, &s, 0);
    json_object_put(s.values);
    return out;
}
json_object *debug_redact(json_object *value, json_object *rule)
{
    return debug_redact_with_context(value, rule, value);
}
