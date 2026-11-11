#include "rule/rule.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static json_object *parse(const char *raw)
{
    json_object *v = json_tokener_parse(raw);
    assert(v);
    return v;
}
static void equal(json_object *value, const char *expected)
{
    json_object *want = parse(expected);
    assert(json_object_equal(value, want));
    json_object_put(want);
}
static void schemas(void)
{
    char e[SP_ERROR] = "";
    json_object *schema =
        parse("{\"type\":\"object\",\"properties\":{\"flag\":{\"type\":\"boolean\",\"default\":"
              "true},\"maybe\":{\"type\":[\"null\",\"integer\"],\"default\":4},\"nested\":{"
              "\"type\":\"object\",\"default\":{},\"properties\":{\"id\":{\"type\":\"integer\","
              "\"default\":7}},\"required\":[\"id\"],\"additionalProperties\":false}},\"required\":"
              "[\"flag\",\"nested\"],\"additionalProperties\":false}");
    assert(!flow_schema_validate(schema, e, sizeof(e)));
    json_object *step = json_object_new_object(),
                *ctx =
                    parse("{\"input\":{\"flag\":false,\"maybe\":null},\"steps\":{\"stable\":1}}");
    json_object_object_add(step, "input_schema", json_object_get(schema));
    assert(!flow_input_prepare(step, ctx, e, sizeof(e)));
    equal(jg(ctx, "input"), "{\"flag\":false,\"maybe\":null,\"nested\":{\"id\":7}}");
    equal(jg(ctx, "steps"), "{\"stable\":1}");
    json_object_object_add(jg(ctx, "input"), "extra", json_object_new_string("DO_NOT_LEAK"));
    json_object *before = flow_clone(ctx);
    assert(flow_input_prepare(step, ctx, e, sizeof(e)) < 0 && strstr(e, "/input/extra") &&
           !strstr(e, "DO_NOT_LEAK"));
    assert(json_object_equal(before, ctx));
    json_object_put(before);
    json_object_put(ctx);
    json_object_put(step);
    json_object_put(schema);
    const char *invalid[] = {"{\"type\":\"float\"}",
                             "{\"type\":[]}",
                             "{\"type\":[\"integer\",false]}",
                             "{\"required\":[2]}",
                             "{\"properties\":{\"x\":4}}",
                             "{\"additionalProperties\":2}",
                             "{\"minimum\":0}",
                             "{\"type\":\"integer\",\"default\":\"1\"}"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); i++) {
        schema = parse(invalid[i]);
        assert(flow_schema_validate(schema, e, sizeof(e)) < 0 && *e);
        json_object_put(schema);
    }
    step =
        parse("{\"input\":{\"selected\":\"{{input.id}}\"},\"input_schema\":{\"type\":\"object\","
              "\"properties\":{\"selected\":{\"type\":\"integer\"}},\"required\":[\"selected\"]}}");
    ctx = parse("{\"input\":{\"id\":8},\"run\":{}}");
    assert(!flow_input_prepare(step, ctx, e, sizeof(e)));
    equal(jg(ctx, "input"), "{\"selected\":8}");
    json_object_put(step);
    json_object_put(ctx);
    step = parse("{\"input\":\"{{input.id}}\",\"input_schema\":{\"type\":\"integer\"}}");
    ctx = parse("{\"input\":{\"id\":\"SECRET\"}}");
    before = flow_clone(ctx);
    assert(flow_input_prepare(step, ctx, e, sizeof(e)) < 0 && strstr(e, "/input") &&
           !strstr(e, "SECRET"));
    assert(json_object_equal(ctx, before));
    json_object_put(step);
    json_object_put(ctx);
    json_object_put(before);
}
static json_object *run(const char *step_raw, const char *context_raw)
{
    char e[SP_ERROR] = "";
    json_object *s = parse(step_raw), *ctx = parse(context_raw), *out = NULL,
                *before = flow_clone(ctx);
    assert(!flow_transform_validate(s, e, sizeof(e)));
    int rc = flow_transform(s, ctx, &out, e, sizeof(e));
    if (rc)
        fprintf(stderr, "transform failed: %s\n", e);
    assert(!rc && json_object_equal(ctx, before));
    json_object_put(before);
    json_object_put(s);
    json_object_put(ctx);
    return out;
}
static void pipelines(void)
{
    json_object *out =
        run("{\"operations\":[{\"op\":\"pick\",\"path\":\"/a~1b/"
            "~0x\"},{\"op\":\"trim\"},{\"op\":\"replace\",\"old\":\"old\",\"new\":\"new\"}]}",
            "{\"input\":{\"a/b\":{\"~x\":\"  old-old  \"}}}");
    assert(!strcmp(json_object_get_string(out), "new-new"));
    json_object_put(out);
    out = run("{\"operations\":[{\"op\":\"pick\",\"path\":\"/"
              "missing\",\"default\":{\"literal\":\"{{input.unresolved}}\"}}]}",
              "{\"input\":{}}");
    equal(out, "{\"literal\":\"{{input.unresolved}}\"}");
    json_object_put(out);
    out = run("{\"operations\":[{\"op\":\"pick\",\"path\":\"/"
              "a\",\"default\":99},{\"op\":\"default\",\"value\":3}]}",
              "{\"input\":{\"a\":null}}");
    assert(json_object_get_int(out) == 3);
    json_object_put(out);
    out = run("{\"operations\":[{\"op\":\"pick\",\"path\":\"/"
              "a\",\"default\":99},{\"op\":\"default\",\"value\":3}]}",
              "{\"input\":{\"a\":false}}");
    assert(json_object_is_type(out, json_type_boolean) && !json_object_get_boolean(out));
    json_object_put(out);
    out = run("{\"operations\":[{\"op\":\"rename\",\"from\":\"a\",\"to\":\"b\"}]}",
              "{\"input\":{\"a\":null}}");
    equal(out, "{\"b\":null}");
    json_object_put(out);
    out = run("{\"operations\":[{\"op\":\"filter\",\"test\":{\"left\":\"{{input.ok}}\",\"op\":"
              "\"eq\",\"right\":true}},{\"op\":\"map\",\"fields\":{\"v\":\"{{input.value}}\","
              "\"parent\":\"{{run.id}}\"}}]}",
              "{\"input\":[{\"ok\":true,\"value\":{\"n\":1}},{\"ok\":false,\"value\":2},{\"ok\":"
              "true,\"value\":null}],\"run\":{\"id\":4},\"steps\":{}}");
    equal(out, "[{\"v\":{\"n\":1},\"parent\":4},{\"v\":null,\"parent\":4}]");
    json_object_put(out);
    const char *casts[] = {
        "{\"value\":\"9223372036854775807\",\"operations\":[{\"op\":\"cast\",\"type\":\"integer\"}]"
        "}",
        "{\"value\":\"1.25e2\",\"operations\":[{\"op\":\"cast\",\"type\":\"number\"}]}",
        "{\"value\":\"false\",\"operations\":[{\"op\":\"cast\",\"type\":\"boolean\"}]}"};
    out = run(casts[0], "{\"input\":0}");
    assert(json_object_get_int64(out) == INT64_MAX);
    json_object_put(out);
    out = run(casts[1], "{\"input\":0}");
    assert(json_object_get_double(out) == 125.0);
    json_object_put(out);
    out = run(casts[2], "{\"input\":0}");
    assert(json_object_is_type(out, json_type_boolean) && !json_object_get_boolean(out));
    json_object_put(out);
}
static void failures(void)
{
    const char *invalid[] = {"{\"operations\":[]}", "{\"operations\":[{\"op\":\"eval\"}]}",
                             "{\"operations\":[{\"op\":\"replace\",\"old\":\"\",\"new\":\"x\"}]}",
                             "{\"operations\":[{\"op\":\"pick\",\"path\":\"/bad~2\"}]}",
                             "{\"operations\":[{\"op\":\"cast\",\"type\":\"array\"}]}"};
    char e[SP_ERROR] = "";
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); i++) {
        json_object *s = parse(invalid[i]);
        assert(flow_transform_validate(s, e, sizeof(e)) < 0 && *e);
        json_object_put(s);
    }
    const char *bad[] = {
        "{\"operations\":[{\"op\":\"rename\",\"from\":\"a\",\"to\":\"b\"}]}",
        "{\"value\":\"9223372036854775808\",\"operations\":[{\"op\":\"cast\",\"type\":\"integer\"}]"
        "}",
        "{\"value\":\"1e9999\",\"operations\":[{\"op\":\"cast\",\"type\":\"number\"}]}",
        "{\"value\":1.5,\"operations\":[{\"op\":\"cast\",\"type\":\"integer\"}]}",
        "{\"value\":\" 1\",\"operations\":[{\"op\":\"cast\",\"type\":\"integer\"}]}",
        "{\"operations\":[{\"op\":\"pick\",\"path\":\"/missing\"}]}"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
        json_object *s = parse(bad[i]), *ctx = parse("{\"input\":{\"a\":1,\"b\":null}}"),
                    *before = flow_clone(ctx), *out = (void *)1;
        assert(flow_transform(s, ctx, &out, e, sizeof(e)) < 0 && !out && *e);
        assert(json_object_equal(ctx, before));
        json_object_put(s);
        json_object_put(ctx);
        json_object_put(before);
    }
}
static void limits(void)
{
    char e[SP_ERROR] = "";
    json_object *ctx = json_object_new_object(), *a = json_object_new_array(),
                *step =
                    parse("{\"operations\":[{\"op\":\"map\",\"fields\":{\"v\":\"{{input}}\"}}]}"),
                *out = NULL;
    for (int i = 0; i < 1001; i++)
        json_object_array_add(a, json_object_new_int(i));
    json_object_object_add(ctx, "input", a);
    assert(flow_transform(step, ctx, &out, e, sizeof(e)) < 0 && !out);
    json_object_put(ctx);
    json_object_put(step);
    ctx = parse("{\"input\":{}}");
    step = parse("{\"operations\":[{\"op\":\"default\",\"value\":0}]}");
    json_object *deep = json_object_new_object(), *cursor = deep;
    for (int i = 0; i < 34; i++) {
        json_object *child = json_object_new_object();
        json_object_object_add(cursor, "x", child);
        cursor = child;
    }
    json_object_object_add(ctx, "input", deep);
    assert(flow_transform(step, ctx, &out, e, sizeof(e)) < 0 && !out);
    json_object_put(ctx);
    json_object_put(step);
    ctx = parse("{\"input\":\"a\"}");
    step = parse(
        "{\"operations\":[{\"op\":\"replace\",\"old\":\"a\",\"new\":\"aaaaaaaaaaaaaaaa\"},{\"op\":"
        "\"replace\",\"old\":\"a\",\"new\":\"aaaaaaaaaaaaaaaa\"},{\"op\":\"replace\",\"old\":\"a\","
        "\"new\":\"aaaaaaaaaaaaaaaa\"},{\"op\":\"replace\",\"old\":\"a\",\"new\":"
        "\"aaaaaaaaaaaaaaaa\"},{\"op\":\"replace\",\"old\":\"a\",\"new\":\"aaaaaaaaaaaaaaaa\"},{"
        "\"op\":\"replace\",\"old\":\"a\",\"new\":\"aaaaaaaaaaaaaaaa\"}]}");
    assert(flow_transform(step, ctx, &out, e, sizeof(e)) < 0 && !out);
    json_object_put(ctx);
    json_object_put(step);
}
static void expanded_defaults(void)
{
    char e[SP_ERROR] = "";
    json_object *
        s = parse(
           "{\"type\":\"object\",\"default\":{},\"properties\":{\"rows\":{\"type\":\"array\","
           "\"items\":{\"type\":\"object\",\"properties\":{\"padding\":{\"type\":\"string\"}}}}}}"),
       *rows_schema = jg(jg(s, "properties"), "rows"), *a = json_object_new_array();
    for (int i = 0; i < 1000; i++)
        json_object_array_add(a, json_object_new_object());
    json_object_object_add(rows_schema, "default", a);
    char padding[5001];
    memset(padding, 'x', 5000);
    padding[5000] = 0;
    json_object_object_add(jg(jg(jg(rows_schema, "items"), "properties"), "padding"), "default",
                           json_object_new_string(padding));
    assert(flow_schema_validate(s, e, sizeof(e)) < 0 && *e);
    json_object_put(s);
}
static void presence_items_and_isolation(void)
{
    char e[SP_ERROR] = "";
    json_object *out = run("{\"operations\":[{\"op\":\"pick\",\"path\":\"/0\",\"default\":99}]}",
                           "{\"input\":[null,false,0]}");
    assert(!out);
    out = run("{\"operations\":[{\"op\":\"pick\",\"path\":\"/1\",\"default\":99}]}",
              "{\"input\":[null,false,0]}");
    assert(json_object_is_type(out, json_type_boolean) && !json_object_get_boolean(out));
    json_object_put(out);
    json_object
        *step = parse(
            "{\"input\":{\"$ref\":\"/steps/lookup/"
            "a~1b~0c\"},\"input_schema\":{\"type\":\"array\",\"items\":{\"type\":\"object\","
            "\"properties\":{\"n\":{\"type\":\"integer\",\"default\":0}},\"required\":[\"n\"]}}}"),
        *ctx = parse("{\"input\":{},\"steps\":{\"lookup\":{\"a/b~c\":[{}, {\"n\":2}]}}}");
    assert(!flow_input_prepare(step, ctx, e, sizeof(e)));
    equal(jg(ctx, "input"), "[{\"n\":0},{\"n\":2}]");
    equal(jg(jg(jg(ctx, "steps"), "lookup"), "a/b~c"), "[{}, {\"n\":2}]");
    json_object_put(step);
    json_object_put(ctx);
    step = parse("{\"input_schema\":{\"type\":\"integer\",\"default\":4}}");
    ctx = parse("{}");
    assert(!flow_input_prepare(step, ctx, e, sizeof(e)));
    assert(ji(ctx, "input", 0) == 4);
    json_object_object_add(ctx, "input", NULL);
    assert(flow_input_prepare(step, ctx, e, sizeof(e)) < 0 && !jg(ctx, "input"));
    json_object_put(step);
    json_object_put(ctx);
    step = parse("{\"input_schema\":{\"type\":\"object\",\"properties\":{\"a\":{\"default\":1}},"
                 "\"required\":[\"missing\"]}}");
    ctx = parse("{\"input\":{}}");
    assert(flow_input_prepare(step, ctx, e, sizeof(e)) < 0 && strstr(e, "/input/missing"));
    equal(jg(ctx, "input"), "{}");
    json_object_put(step);
    json_object_put(ctx);
    step = parse(
        "{\"operations\":[{\"op\":\"map\",\"fields\":{\"nested\":{\"$ref\":\"/input/nested\"}}}]}");
    ctx = parse("{\"input\":[{\"nested\":{\"value\":1}},{\"nested\":{\"value\":2}}]}");
    assert(!flow_transform(step, ctx, &out, e, sizeof(e)));
    json_object_object_add(jg(json_object_array_get_idx(out, 0), "nested"), "value",
                           json_object_new_int(99));
    equal(jg(ctx, "input"), "[{\"nested\":{\"value\":1}},{\"nested\":{\"value\":2}}]");
    assert(ji(jg(json_object_array_get_idx(out, 1), "nested"), "value", 0) == 2);
    json_object_put(out);
    json_object_put(ctx);
    json_object_put(step);
    step = parse("{\"operations\":[{\"op\":\"filter\",\"test\":{\"left\":\"{{input.bad}}\",\"op\":"
                 "\"eq\",\"right\":1}}]}");
    ctx = parse("{\"input\":[{}]}");
    assert(flow_transform(step, ctx, &out, e, sizeof(e)) < 0 && !out && strstr(e, "/operations/0"));
    json_object_put(step);
    json_object_put(ctx);
    step = parse("{\"type\":\"integer\\u0000anything\"}");
    assert(flow_schema_validate(step, e, sizeof(e)) < 0);
    json_object_put(step);
}
static void operation_and_array_edges(void)
{
    char e[SP_ERROR] = "";
    json_object *s = parse("{\"operations\":[]}"), *ops = jg(s, "operations");
    for (int i = 0; i < 32; i++)
        json_object_array_add(ops, parse("{\"op\":\"default\",\"value\":null}"));
    assert(!flow_transform_validate(s, e, sizeof(e)));
    json_object_array_add(ops, parse("{\"op\":\"trim\"}"));
    assert(flow_transform_validate(s, e, sizeof(e)) < 0);
    json_object_put(s);
    s = parse("{\"operations\":[{\"op\":\"map\",\"fields\":{\"n\":\"{{input}}\"}}]}");
    json_object *ctx = parse("{\"input\":[]}"), *a = jg(ctx, "input"), *out = NULL;
    for (int i = 0; i < 1000; i++)
        json_object_array_add(a, json_object_new_int(i));
    assert(!flow_transform(s, ctx, &out, e, sizeof(e)) && json_object_array_length(out) == 1000);
    assert(ji(json_object_array_get_idx(out, 999), "n", 0) == 999);
    json_object_put(s);
    json_object_put(ctx);
    json_object_put(out);
    out = run("{\"operations\":[{\"op\":\"replace\",\"old\":\"aa\",\"new\":\"b\"}]}",
              "{\"input\":\"aa\\u0000aa\"}");
    assert(json_object_get_string_len(out) == 3 && !memcmp(json_object_get_string(out), "b\0b", 3));
    json_object_put(out);
}
static void unrelated_history_arrays(void)
{
    char e[SP_ERROR] = "";
    json_object *ctx = parse("{\"input\":{\"id\":5},\"steps\":{\"previous\":{\"items\":[]}}}"),
                *items = jg(jg(jg(ctx, "steps"), "previous"), "items");
    for (int i = 0; i < 1001; i++) {
        json_object *item = json_object_new_object();
        json_object_object_add(item, "id", json_object_new_int(i));
        json_object_array_add(items, item);
    }
    json_object *step = parse("{\"value\":\" hi \",\"operations\":[{\"op\":\"trim\"}]}"),
                *out = NULL, *before = flow_clone(ctx);
    assert(!flow_transform(step, ctx, &out, e, sizeof(e)));
    assert(!strcmp(json_object_get_string(out), "hi") && json_object_equal(before, ctx));
    json_object_put(out);
    json_object_put(before);
    json_object_put(step);
    step = parse("{\"input\":{\"selected\":{\"$ref\":\"/steps/previous/items/1000/"
                 "id\"}},\"input_schema\":{\"type\":\"object\",\"properties\":{\"selected\":{"
                 "\"type\":\"integer\"}},\"required\":[\"selected\"]}}");
    assert(!flow_input_prepare(step, ctx, e, sizeof(e)));
    equal(jg(ctx, "input"), "{\"selected\":1000}");
    assert(json_object_array_length(items) == 1001);
    json_object_put(step);
    step = parse("{\"input_schema\":{\"type\":\"object\",\"properties\":{\"selected\":{\"type\":"
                 "\"integer\"}}}}");
    assert(!flow_input_prepare(step, ctx, e, sizeof(e)));
    json_object_put(step);
    step = parse("{\"value\":{\"$ref\":\"/steps/previous/"
                 "items\"},\"operations\":[{\"op\":\"default\",\"value\":[]}]}");
    assert(flow_transform(step, ctx, &out, e, sizeof(e)) < 0 && !out && strstr(e, "/value"));
    json_object_put(step);
    step = parse(
        "{\"input\":{\"$ref\":\"/steps/previous/items\"},\"input_schema\":{\"type\":\"array\"}}");
    before = flow_clone(ctx);
    assert(flow_input_prepare(step, ctx, e, sizeof(e)) < 0 && strstr(e, "/input"));
    assert(json_object_equal(before, ctx));
    json_object_put(before);
    json_object_put(step);
    step = parse("{\"value\":\" hi \",\"operations\":[{\"op\":\"trim\"}]}");
    json_object *deep = json_object_new_object(), *cursor = deep;
    for (int i = 0; i < 34; i++) {
        json_object *child = json_object_new_object();
        json_object_object_add(cursor, "nested", child);
        cursor = child;
    }
    json_object_object_add(jg(ctx, "steps"), "unrelated", deep);
    assert(flow_transform(step, ctx, &out, e, sizeof(e)) < 0 && !out && strstr(e, "nesting"));
    json_object_object_del(jg(ctx, "steps"), "unrelated");
    char *large = malloc(4 * 1024 * 1024 + 1);
    assert(large);
    memset(large, 'x', 4 * 1024 * 1024);
    large[4 * 1024 * 1024] = 0;
    json_object_object_add(jg(ctx, "steps"), "unrelated", json_object_new_string(large));
    free(large);
    assert(flow_transform(step, ctx, &out, e, sizeof(e)) < 0 && !out && strstr(e, "4 MiB"));
    json_object_put(step);
    json_object_put(ctx);
}
int main(void)
{
    unrelated_history_arrays();
    operation_and_array_edges();
    presence_items_and_isolation();
    expanded_defaults();
    schemas();
    pipelines();
    failures();
    limits();
    puts("data helper tests passed");
    return 0;
}
