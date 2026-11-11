#include "rule/rule.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
int main(void)
{
    char e[512] = {0};
    json_object *old = json_tokener_parse(
        "{\"name\":\"Old {{literal}}\",\"url\":\"https://example.com/\",\"method\":\"POST\","
        "\"body\":\"{{literal body}}\",\"headers\":{\"X.Token\":\"{{literal token}}\"},"
        "\"max_pages\":3,\"interval_seconds\":60,\"enabled\":false,\"delay_ms\":7,"
        "\"fields\":[{\"name\":\"file.with "
        "dot\",\"selector\":\"a\",\"attribute\":\"href\",\"download\":true},"
        "{\"name\":\"price\",\"selector\":\"b\",\"type\":\"number\",\"required\":true}]}");
    assert(old);
    json_object *converted = rule_upgrade_v1(old, e, sizeof(e));
    assert(converted && rule_validate(converted, e, sizeof(e)) == 0);
    assert(ji(old, "schema_version", 1) == 1 && !jg(old, "steps"));
    assert(ji(converted, "interval_seconds", 0) == 60 && !jb(converted, "enabled", true));
    assert(ji(jg(converted, "defaults"), "delay_ms", 0) == 7);
    json_object *page = flow_step(converted, "page");
    assert(!strcmp(js(jg(page, "request"), "method", ""), "POST"));
    assert(jb(page, "retry_post", false)); /* preserves explicit legacy replay policy */
    json_object *ctx = json_object_new_object(), *rendered = NULL;
    json_object_object_add(ctx, "run", flow_clone(jg(converted, "vars")));
    json_object_object_add(
        ctx, "input",
        json_tokener_parse("{\"url\":\"https://example.com/page2\",\"field_0\":\"https://"
                           "example.com/a\",\"field_1\":2.5}"));
    assert(flow_render(jg(page, "request"), ctx, &rendered, e, sizeof(e)) == 0);
    assert(!strcmp(js(rendered, "body", ""), "{{literal body}}"));
    assert(!strcmp(js(jg(rendered, "headers"), "X.Token", ""), "{{literal token}}"));
    assert(!strcmp(js(rendered, "headers_origin", ""), "https://example.com/"));
    assert(!strcmp(js(rendered, "body_origin", ""), "https://example.com/"));
    json_object_put(rendered);
    assert(flow_render(jg(flow_step(converted, "save"), "fields"), ctx, &rendered, e, sizeof(e)) ==
           0);
    assert(!strcmp(js(rendered, "file.with dot", ""), "https://example.com/a"));
    assert(json_object_get_double(jg(rendered, "price")) == 2.5);
    json_object_put(rendered);
    assert(ji(jg(flow_step(converted, "limit"), "test"), "right", 0) == 3);
    assert(jg(flow_step(converted, "download_0"), "headers"));
    json_object_put(converted);
    json_object_object_add(old, "url", json_object_new_string("file:///etc/passwd"));
    assert(!rule_upgrade_v1(old, e, sizeof(e)));
    json_object_put(old);
    json_object_put(ctx);
    puts("upgrade tests passed");
    return 0;
}
