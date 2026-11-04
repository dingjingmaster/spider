#include "storage/storage.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
int main(void)
{
    char path[] = "/tmp/spider-test-XXXXXX", err[512] = {0};
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    Store s;
    assert(store_open(&s, path, err, sizeof(err)) == 0);
    json_object *r = json_tokener_parse(
        "{\"name\":\"first\",\"url\":\"http://localhost/\",\"fields\":[{\"name\":\"x\"}]}");
    int64_t id = store_save_spider(&s, 0, r, err, sizeof(err));
    assert(id > 0);
    int64_t run = store_start_run(&s, id, err, sizeof(err));
    assert(run > 0);
    assert(store_start_run(&s, id, err, sizeof(err)) < 0);
    json_object_object_add(r, "name", json_object_new_string("edited"));
    assert(store_save_spider(&s, id, r, err, sizeof(err)) == id);
    json_object *claimed = store_claim_run(&s);
    assert(claimed);
    assert(!strcmp(js(jg(claimed, "snapshot"), "name", ""), "first"));
    json_object_put(claimed);
    int64_t req = store_enqueue(&s, run, "http://localhost/");
    assert(req > 0);
    claimed = store_claim_request(&s, run, err, sizeof(err));
    assert(claimed);
    json_object_put(claimed);
    store_close(&s);
    assert(store_open(&s, path, err, sizeof(err)) == 0);
    claimed = store_claim_run(&s);
    assert(claimed);
    json_object_put(claimed);
    claimed = store_claim_request(&s, run, err, sizeof(err));
    assert(claimed);
    assert(ji(claimed, "attempts", 0) == 2);
    json_object_put(claimed);
    assert(store_cancel_run(&s, run) == 0);
    assert(store_cancelled(&s, run));
    assert(store_finish_run(&s, run, "cancelled", "") == 0);
    assert(store_start_run(&s, id, err, sizeof(err)) > 0);
    json_object_put(r);
    store_close(&s);
    unlink(path);
    puts("storage tests passed");
    return 0;
}
