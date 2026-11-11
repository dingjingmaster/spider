#ifndef SPIDER_COMMON_H
#define SPIDER_COMMON_H
#include <json-c/json.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define SP_ERROR 512
#define SP_PATH 4096
const char *js(json_object *obj, const char *key, const char *fallback);
int ji(json_object *obj, const char *key, int fallback);
bool jb(json_object *obj, const char *key, bool fallback);
json_object *jg(json_object *obj, const char *key);
json_object *json_parse(const char *text, char *error, size_t cap);
/* Success can produce NULL for JSON null. */
int json_value_parse(const char *text, json_object **value, char *error, size_t cap);
/* RFC 6901 lookup; success includes an explicitly present JSON null. */
int json_pointer_lookup(json_object *root, const char *pointer, json_object **value);
int fail(char *error, size_t cap, const char *fmt, ...);
int mkdirs(const char *path);
char *url_resolve(const char *base, const char *relative);
bool url_valid(const char *url);
bool url_same_origin(const char *a, const char *b);
int64_t now_ms(void);
void sleep_ms(int milliseconds);
char *trim_copy(const char *text, size_t len);
int sha256_file(const char *path, char hex[65]);
#endif
