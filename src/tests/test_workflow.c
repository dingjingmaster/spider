#include "rule/workflow.h"
#include "rule/rule.h"
#include "engine/workflow.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
int main(void)
{
    char e[512] = {0};
    json_object *r = json_tokener_parse(
        "{\"schema_version\":2,\"name\":\"flow\",\"entry\":\"list\",\"steps\":[{\"id\":\"list\","
        "\"type\":\"request\",\"request\":{\"url\":\"https://example.com/"
        "\"},\"next\":\"save\"},{\"id\":\"save\",\"type\":\"emit\",\"fields\":{\"id\":\"{{input.id}"
        "}\"}}]}");
    assert(rule_validate(r, e, sizeof(e)) == 0);
    json_object_object_add(r, "sensitive_fields", json_object_new_string("token"));
    assert(rule_validate(r, e, sizeof(e)) < 0);
    json_object_object_add(r, "sensitive_fields", json_tokener_parse("[\"customToken\"]"));
    assert(rule_validate(r, e, sizeof(e)) == 0);
    json_object *req = jg(flow_step(r, "list"), "request");
    json_object_object_add(req, "body", json_object_new_string(""));
    json_object_object_add(req, "json", NULL);
    assert(rule_validate(r, e, sizeof(e)) < 0);
    json_object_object_del(req, "body");
    json_object_object_del(req, "json");
    json_object *supplied = json_tokener_parse("{\"input\":null}"),
                *initial_ctx = workflow_context(r, supplied), *null_input = NULL;
    assert(json_object_object_get_ex(initial_ctx, "input", &null_input) && !null_input);
    json_object_put(initial_ctx);
    json_object_put(supplied);
    json_object_object_add(r, "vars", json_tokener_parse("{\"literal\":\"{{untouched}}\"}"));
    assert(rule_validate(r, e, sizeof(e)) == 0);
    json_object_object_add(flow_step(r, "list"), "next", json_object_new_string("missing"));
    assert(rule_validate(r, e, sizeof(e)) < 0);
    json_object_object_add(flow_step(r, "list"), "next", json_object_new_string("save"));
    json_object *ctx = json_tokener_parse(
        "{\"run\":{\"q\":\"a&b "
        "c\"},\"input\":{\"id\":12},\"steps\":{\"detail\":{\"items\":[{\"name\":\"Alpha\"}]}}}");
    json_object_object_add(jg(ctx, "input"), "a.b/~", json_tokener_parse("[false,null,{\"n\":7}]"));
    json_object *ref = json_tokener_parse("{\"$ref\":\"/input/a.b~1~0\"}"), *ref_out = NULL;
    assert(flow_render(ref, ctx, &ref_out, e, sizeof(e)) == 0);
    assert(json_object_is_type(ref_out, json_type_array));
    assert(json_object_get_int(jg(json_object_array_get_idx(ref_out, 2), "n")) == 7);
    json_object_put(ref_out);
    json_object_put(ref);
    const char literal_bytes[] = {'a', 'a', '\0', 'a', 'a'};
    ref = json_object_new_string_len(literal_bytes, sizeof(literal_bytes));
    assert(flow_render(ref, ctx, &ref_out, e, sizeof(e)) == 0);
    assert(json_object_get_string_len(ref_out) == 5);
    json_object_put(ref_out);
    json_object_put(ref);
    ref = json_tokener_parse("{\"$ref\":\"/input/absent\",\"default\":\"{{literal}}\"}");
    assert(flow_render(ref, ctx, &ref_out, e, sizeof(e)) == 0);
    assert(!strcmp(json_object_get_string(ref_out), "{{literal}}"));
    json_object_put(ref_out);
    json_object_put(ref);
    ref = json_tokener_parse("{\"$ref\":\"/input/a.b~1~0/1\",\"default\":10}");
    assert(flow_render(ref, ctx, &ref_out, e, sizeof(e)) == 0 && !ref_out);
    json_object_put(ref);
    ref = json_tokener_parse("{\"$ref\":\"/input/absent\"}");
    assert(flow_render(ref, ctx, &ref_out, e, sizeof(e)) < 0);
    json_object_put(ref);
    json_object *v = json_object_new_string("{{input.id}}"), *out = NULL;
    assert(flow_render(v, ctx, &out, e, sizeof(e)) == 0);
    assert(json_object_is_type(out, json_type_int));
    assert(json_object_get_int(out) == 12);
    json_object_put(out);
    json_object_put(v);
    v = json_object_new_string("ID={{input.id}}:{{steps.detail.items.0.name}}");
    assert(flow_render(v, ctx, &out, e, sizeof(e)) == 0);
    assert(!strcmp(json_object_get_string(out), "ID=12:Alpha"));
    json_object_put(out);
    json_object_put(v);
    v = json_object_new_string("{{input.unknown}}");
    assert(flow_render(v, ctx, &out, e, sizeof(e)) < 0);
    json_object_put(v);
    v = json_object_new_string("{{input.id");
    assert(flow_render(v, ctx, &out, e, sizeof(e)) < 0);
    json_object_put(v);
    json_object *test = json_tokener_parse("{\"left\":\"{{input.absent}}\",\"op\":\"exists\"}");
    bool result = true;
    assert(flow_test(test, ctx, &result, e, sizeof(e)) == 0 && !result);
    json_object_put(test);
    test = json_tokener_parse("{\"left\":{\"$ref\":\"/input/absent\"},\"op\":\"exists\"}");
    assert(flow_test(test, ctx, &result, e, sizeof(e)) == 0 && !result);
    json_object_put(test);
    test = json_tokener_parse("{\"left\":\"{{input.id}}\",\"op\":\"gt\",\"right\":10}");
    assert(flow_test(test, ctx, &result, e, sizeof(e)) == 0 && result);
    json_object_put(test);
    json_object_object_add(jg(ctx, "run"), "nullable", NULL);
    v = json_object_new_string("{{run.nullable}}");
    assert(flow_render(v, ctx, &out, e, sizeof(e)) == 0 && out == NULL);
    json_object_put(v);
    char *large = malloc(65537);
    memset(large, 'x', 65536);
    large[65536] = 0;
    json_object_object_add(jg(ctx, "run"), "large", json_object_new_string(large));
    free(large);
    v = json_object_new_object();
    for (int i = 0; i < 80; i++) {
        char key[20];
        snprintf(key, sizeof(key), "field%d", i);
        json_object_object_add(v, key, json_object_new_string("{{run.large}}"));
    }
    assert(flow_render(v, ctx, &out, e, sizeof(e)) < 0 && out == NULL);
    json_object_put(v);
    json_object *nested = json_object_new_int(1);
    for (int i = 0; i < 25; i++) {
        json_object *wrapper = json_object_new_object();
        json_object_object_add(wrapper, "n", nested);
        nested = wrapper;
    }
    json_object_object_add(jg(ctx, "input"), "nested", nested);
    v = json_tokener_parse("{\"$ref\":\"/input/nested\"}");
    for (int i = 0; i < 10; i++) {
        json_object *wrapper = json_object_new_object();
        json_object_object_add(wrapper, "n", v);
        v = wrapper;
    }
    assert(flow_render(v, ctx, &out, e, sizeof(e)) < 0 && !out);
    json_object_put(v);
    json_object_put(ctx);
    json_object_put(r);
    puts("workflow rule tests passed");
    return 0;
}
