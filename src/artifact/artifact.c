#include "artifact.h"
#include "http/http.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
int artifact_download(App *app, int64_t run, json_object *rule, const char *url, cancel_fn cancel,
                      void *ctx, char *e, size_t n)
{
    json_object *existing =
        store_query(&app->store, "SELECT url,path FROM artifacts WHERE run_id=?", run, e, n);
    if (!existing)
        return -1;
    for (size_t i = 0; i < json_object_array_length(existing); i++) {
        json_object *a = json_object_array_get_idx(existing, i);
        if (!strcmp(js(a, "url", ""), url) && access(js(a, "path", ""), R_OK) == 0) {
            json_object_put(existing);
            return 0;
        }
    }
    json_object_put(existing);
    char temp[SP_PATH], final[SP_PATH];
    if (snprintf(temp, sizeof(temp), "%s/downloads/.part-XXXXXX", app->data_dir) >=
        (int)sizeof(temp))
        return fail(e, n, "Download path too long");
    int fd = mkstemp(temp);
    if (fd < 0)
        return fail(e, n, "Cannot create download file");
    FILE *f = fdopen(fd, "wb");
    if (!f) {
        close(fd);
        unlink(temp);
        return fail(e, n, "Cannot open download stream");
    }
    HttpResponse response = {0};
    int rc = http_fetch(rule, url, f, cancel, ctx, &response, e, n);
    if (fflush(f) || ferror(f) || fsync(fd))
        rc = fail(e, n, "Cannot persist download");
    if (fclose(f))
        rc = fail(e, n, "Cannot close download");
    if (rc) {
        unlink(temp);
        http_response_free(&response);
        return -1;
    }
    char hash[65];
    if (sha256_file(temp, hash)) {
        unlink(temp);
        http_response_free(&response);
        return fail(e, n, "Cannot hash download");
    }
    if (snprintf(final, sizeof(final), "%s/downloads/%lld-%s.bin", app->data_dir, (long long)run,
                 hash) >= (int)sizeof(final)) {
        unlink(temp);
        http_response_free(&response);
        return fail(e, n, "Download path too long");
    }
    bool existed = access(final, F_OK) == 0;
    if (rename(temp, final)) {
        unlink(temp);
        http_response_free(&response);
        return fail(e, n, "Cannot finalize download");
    }
    int64_t id = store_add_artifact(&app->store, run, url, final, hash, (int64_t)response.size);
    http_response_free(&response);
    if (id < 0) {
        if (!existed)
            unlink(final);
        return fail(e, n, "Cannot register download in SQLite");
    }
    return 0;
}
