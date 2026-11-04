#include "rule/rule.h"
#include "extract/extract.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static json_object *parse(const char *s)
{
    json_object *j = json_tokener_parse(s);
    assert(j);
    return j;
}
int main(void)
{
    char error[512] = {0};
    json_object *bad = parse("{\"name\":\"bad\",\"url\":\"file:///etc/passwd\"}");
    assert(rule_validate(bad, error, sizeof(error)) < 0);
    json_object_put(bad);
    json_object *rule = parse("{\"name\":\"example\",\"url\":\"http://localhost/"
                              "\",\"format\":\"html\",\"row_selector\":\".row\",\"fields\":[{"
                              "\"name\":\"title\",\"selector\":\"h2\",\"required\":true},{\"name\":"
                              "\"price\",\"selector\":\"b\",\"type\":\"number\"}]}");
    assert(rule_validate(rule, error, sizeof(error)) < 0); /* only workflows are public */
    assert(request_validate(rule, error, sizeof(error)) == 0);
    json_object *items = NULL;
    char *next = NULL;
    const char *html = "<div class='row'><h2>A &amp; B</h2><b>12.5</b></div><div "
                       "class='row'><h2>Second</h2><b>20</b></div>";
    assert(extract_document(rule, html, "http://localhost/", &items, &next, error, sizeof(error)) ==
           0);
    assert(json_object_array_length(items) == 2);
    json_object *first = json_object_array_get_idx(items, 0), *v = NULL;
    assert(json_object_object_get_ex(first, "title", &v));
    assert(strcmp(json_object_get_string(v), "A & B") == 0);
    assert(json_object_object_get_ex(first, "price", &v));
    assert(json_object_get_double(v) == 12.5);
    json_object_put(items);
    assert(extract_document(rule, "<div class='row'><b>2</b></div>", "http://localhost/", &items,
                            &next, error, sizeof(error)) < 0);
    json_object_object_add(rule, "row_selector", json_object_new_string("["));
    assert(request_validate(rule, error, sizeof(error)) < 0);
    json_object_put(rule);
    rule =
        parse("{\"name\":\"json\",\"url\":\"https://example.com/"
              "\",\"format\":\"json\",\"row_selector\":\"/"
              "rows\",\"fields\":[{\"name\":\"title\",\"selector\":\"/name\",\"required\":true}]}");
    assert(request_validate(rule, error, sizeof(error)) == 0);
    assert(extract_document(rule, "{\"rows\":[{\"name\":\"One\"},{\"name\":\"Two\"}]}",
                            "https://example.com/", &items, &next, error, sizeof(error)) == 0);
    assert(json_object_array_length(items) == 2);
    json_object_put(items);
    assert(extract_document(rule, "not json", "https://example.com/", &items, &next, error,
                            sizeof(error)) < 0);
    json_object_put(rule);
    puts("core tests passed");
    return 0;
}
