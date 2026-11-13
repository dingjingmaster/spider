#include "common.h"
#include <curl/curl.h>
#include <openssl/evp.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>
json_object *jg(json_object *o, const char *k)
{
    json_object *v = NULL;
    if (o)
        json_object_object_get_ex(o, k, &v);
    return v;
}

/* Returns the sequence length implied by a lead byte, or 0 when it cannot start
 * a valid UTF-8 sequence. Continuation bytes are validated by the caller. */
static size_t utf8_sequence_length(unsigned char lead)
{
    if (lead >= 0xC2 && lead <= 0xDF)
        return 2;
    if (lead >= 0xE0 && lead <= 0xEF)
        return 3;
    if (lead >= 0xF0 && lead <= 0xF4)
        return 4;
    return 0;
}

char *utf8_repair(const char *text)
{
    if (!text)
        text = "";
    size_t length = strlen(text);
    if (length > (SIZE_MAX - 1) / 3)
        return NULL;
    char *out = malloc(length * 3 + 1);
    if (!out)
        return NULL;
    const unsigned char *p = (const unsigned char *)text;
    size_t used = 0;
    while (*p) {
        if (*p < 0x80) {
            out[used++] = (char)*p++;
            continue;
        }
        size_t need = utf8_sequence_length(*p);
        size_t i = 1;
        uint32_t cp = need ? (*p & (0x7F >> need)) : 0;
        for (; need && i < need; i++) {
            if ((p[i] & 0xC0) != 0x80)
                break;
            cp = (cp << 6) | (p[i] & 0x3F);
        }
        bool valid = need && i == need && !(need == 3 && cp < 0x800) &&
                     !(need == 4 && (cp < 0x10000 || cp > 0x10FFFF)) &&
                     !(cp >= 0xD800 && cp <= 0xDFFF);
        if (!valid) {
            memcpy(out + used, "\xEF\xBF\xBD", 3);
            used += 3;
            /* Skip the largest invalid subpart so a split character becomes
             * one replacement rather than one per byte. */
            p += i;
            continue;
        }
        memcpy(out + used, p, need);
        used += need;
        p += need;
    }
    out[used] = 0;
    return out;
}

void utf8_trim_incomplete(char *text)
{
    if (!text)
        return;
    size_t length = strlen(text);
    if (!length)
        return;
    size_t start = length;
    while (start > 0 && (((unsigned char)text[start - 1]) & 0xC0) == 0x80)
        start--;
    if (!start)
        return;
    unsigned char lead = (unsigned char)text[start - 1];
    if (lead < 0x80)
        return;
    size_t need = utf8_sequence_length(lead);
    if (!need || length - (start - 1) < need)
        text[start - 1] = 0;
}
const char *js(json_object *o, const char *k, const char *d)
{
    json_object *v = jg(o, k);
    return v && json_object_is_type(v, json_type_string) ? json_object_get_string(v) : d;
}
int ji(json_object *o, const char *k, int d)
{
    json_object *v = jg(o, k);
    return v && json_object_is_type(v, json_type_int) ? json_object_get_int(v) : d;
}
bool jb(json_object *o, const char *k, bool d)
{
    json_object *v = jg(o, k);
    return v && json_object_is_type(v, json_type_boolean) ? json_object_get_boolean(v) : d;
}
int fail(char *e, size_t n, const char *fmt, ...)
{
    if (e && n) {
        va_list a;
        va_start(a, fmt);
        vsnprintf(e, n, fmt, a);
        va_end(a);
    }
    return -1;
}
json_object *json_parse(const char *s, char *e, size_t n)
{
    json_object *value = NULL;
    json_value_parse(s, &value, e, n);
    return value;
}
int json_value_parse(const char *s, json_object **value, char *e, size_t n)
{
    *value = NULL;
    if (!s)
        return fail(e, n, "Missing JSON");
    json_tokener *t = json_tokener_new();
    if (!t)
        return fail(e, n, "Out of memory");
    size_t length = strlen(s);
    json_object *j = json_tokener_parse_ex(t, s, (int)length + 1);
    enum json_tokener_error code = json_tokener_get_error(t);
    size_t end = json_tokener_get_parse_end(t);
    while (end < length && isspace((unsigned char)s[end]))
        end++;
    int rc = 0;
    if (code != json_tokener_success || end < length) {
        fail(e, n, "Invalid JSON: %s", json_tokener_error_desc(code));
        if (j)
            json_object_put(j);
        j = NULL;
        rc = -1;
    }
    json_tokener_free(t);
    *value = j;
    return rc;
}
int json_pointer_lookup(json_object *root, const char *pointer, json_object **value)
{
    *value = NULL;
    if (!pointer || (*pointer && *pointer != '/'))
        return -1;
    json_object *current = root;
    const char *p = pointer;
    while (*p) {
        const char *start = ++p;
        while (*p && *p != '/')
            p++;
        size_t length = (size_t)(p - start);
        char *key = malloc(length + 1);
        if (!key)
            return -1;
        size_t used = 0;
        for (size_t i = 0; i < length; i++) {
            char c = start[i];
            if (c == '~') {
                if (++i == length || (start[i] != '0' && start[i] != '1')) {
                    free(key);
                    return -1;
                }
                c = start[i] == '0' ? '~' : '/';
            }
            key[used++] = c;
        }
        key[used] = 0;
        int rc = -1;
        if (json_object_is_type(current, json_type_object))
            rc = json_object_object_get_ex(current, key, &current) ? 0 : -1;
        else if (json_object_is_type(current, json_type_array) && *key &&
                 !(key[0] == '0' && key[1])) {
            bool digits = true;
            for (size_t i = 0; i < used; i++)
                if (key[i] < '0' || key[i] > '9')
                    digits = false;
            errno = 0;
            unsigned long long idx = digits ? strtoull(key, NULL, 10) : 0;
            if (digits && !errno && idx < json_object_array_length(current)) {
                current = json_object_array_get_idx(current, (size_t)idx);
                rc = 0;
            }
        }
        free(key);
        if (rc)
            return -1;
    }
    *value = current;
    return 0;
}
int mkdirs(const char *p)
{
    char b[SP_PATH];
    if (strlen(p) >= sizeof(b))
        return -1;
    strcpy(b, p);
    for (char *q = b + 1; *q; q++)
        if (*q == '/') {
            *q = 0;
            if (mkdir(b, 0700) && errno != EEXIST)
                return -1;
            *q = '/';
        }
    return mkdir(b, 0700) && errno != EEXIST ? -1 : 0;
}
bool url_valid(const char *s)
{
    if (!s || strlen(s) > 8192 || strchr(s, '\r') || strchr(s, '\n'))
        return false;
    CURLU *u = curl_url();
    if (!u)
        return false;
    char *scheme = NULL, *host = NULL, *user = NULL;
    bool ok = curl_url_set(u, CURLUPART_URL, s, 0) == CURLUE_OK &&
              curl_url_get(u, CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK &&
              curl_url_get(u, CURLUPART_HOST, &host, 0) == CURLUE_OK && host[0] &&
              (!strcmp(scheme, "http") || !strcmp(scheme, "https"));
    if (curl_url_get(u, CURLUPART_USER, &user, 0) == CURLUE_OK && user[0])
        ok = false;
    curl_free(scheme);
    curl_free(host);
    curl_free(user);
    curl_url_cleanup(u);
    return ok;
}
char *url_resolve(const char *base, const char *rel)
{
    CURLU *u = curl_url();
    char *raw = NULL, *out = NULL;
    if (u && rel && curl_url_set(u, CURLUPART_URL, base, 0) == CURLUE_OK &&
        curl_url_set(u, CURLUPART_URL, rel, 0) == CURLUE_OK &&
        curl_url_get(u, CURLUPART_URL, &raw, 0) == CURLUE_OK && url_valid(raw))
        out = strdup(raw);
    curl_free(raw);
    curl_url_cleanup(u);
    return out;
}
bool url_same_origin(const char *a, const char *b)
{
    CURLU *left = curl_url(), *right = curl_url();
    char *ls = NULL, *lh = NULL, *lp = NULL, *rs = NULL, *rh = NULL, *rp = NULL;
    bool same = left && right && curl_url_set(left, CURLUPART_URL, a, 0) == CURLUE_OK &&
                curl_url_set(right, CURLUPART_URL, b, 0) == CURLUE_OK &&
                curl_url_get(left, CURLUPART_SCHEME, &ls, 0) == CURLUE_OK &&
                curl_url_get(left, CURLUPART_HOST, &lh, 0) == CURLUE_OK &&
                curl_url_get(left, CURLUPART_PORT, &lp, CURLU_DEFAULT_PORT) == CURLUE_OK &&
                curl_url_get(right, CURLUPART_SCHEME, &rs, 0) == CURLUE_OK &&
                curl_url_get(right, CURLUPART_HOST, &rh, 0) == CURLUE_OK &&
                curl_url_get(right, CURLUPART_PORT, &rp, CURLU_DEFAULT_PORT) == CURLUE_OK &&
                !strcasecmp(ls, rs) && !strcasecmp(lh, rh) && !strcmp(lp, rp);
    curl_free(ls);
    curl_free(lh);
    curl_free(lp);
    curl_free(rs);
    curl_free(rh);
    curl_free(rp);
    curl_url_cleanup(left);
    curl_url_cleanup(right);
    return same;
}
int64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
void sleep_ms(int n)
{
    if (n <= 0)
        return;
    struct timespec t = {n / 1000, (n % 1000) * 1000000L};
    while (nanosleep(&t, &t) && errno == EINTR) {
    }
}
char *trim_copy(const char *s, size_t n)
{
    while (n && isspace((unsigned char)*s)) {
        s++;
        n--;
    }
    while (n && isspace((unsigned char)s[n - 1]))
        n--;
    char *p = malloc(n + 1);
    if (p) {
        memcpy(p, s, n);
        p[n] = 0;
    }
    return p;
}
int sha256_file(const char *path, char hex[65])
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    EVP_MD_CTX *c = EVP_MD_CTX_new();
    unsigned char b[16384], hash[32];
    unsigned int len = 0;
    int ok = c && EVP_DigestInit_ex(c, EVP_sha256(), NULL) == 1;
    size_t n;
    while (ok && (n = fread(b, 1, sizeof(b), f)))
        ok = EVP_DigestUpdate(c, b, n) == 1;
    if (ferror(f))
        ok = 0;
    if (ok)
        ok = EVP_DigestFinal_ex(c, hash, &len) == 1 && len == 32;
    fclose(f);
    EVP_MD_CTX_free(c);
    if (!ok)
        return -1;
    for (int i = 0; i < 32; i++)
        snprintf(hex + i * 2, 3, "%02x", hash[i]);
    return 0;
}
