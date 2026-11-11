#include "download/download.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

typedef enum { EXISTS_OVERWRITE, EXISTS_SKIP, EXISTS_UNIQUE } ExistsPolicy;

struct DownloadClient {
    char directory[SP_PATH];
    pthread_mutex_t mutex;
};

static int append(char *buffer, size_t cap, const char *text, char *e, size_t n)
{
    size_t used = strlen(buffer), add = strlen(text);
    if (used + add >= cap)
        return fail(e, n, "Download path too long");
    memcpy(buffer + used, text, add + 1);
    return 0;
}

/* One path component: control characters and backslashes become '_'; the
 * component is capped at 255 bytes without splitting a UTF-8 sequence. */
static int sanitize_segment(const char *src, size_t len, char *dst, size_t cap, char *e, size_t n)
{
    if (!len)
        return fail(e, n, "Download path has an empty segment");
    if (len + 1 > cap)
        return fail(e, n, "Download path too long");
    size_t out = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c < 0x20 || c == 0x7f || c == '\\')
            c = '_';
        dst[out++] = (char)c;
    }
    if (out > 255) {
        size_t cut = 255;
        while (cut > 0 && ((unsigned char)dst[cut] & 0xc0) == 0x80)
            cut--;
        out = cut;
    }
    if (!out)
        return fail(e, n, "Download path has an empty segment");
    dst[out] = 0;
    return 0;
}

static int absolute_root(const char *root, char *e, size_t n)
{
    if (!root || root[0] != '/')
        return fail(e, n, "download_root must be an absolute path");
    if (strlen(root) >= SP_PATH)
        return fail(e, n, "Download path too long");
    for (const char *p = root; *p; p++)
        if ((unsigned char)*p < 0x20 || *p == 0x7f)
            return fail(e, n, "download_root contains control characters");
    const char *p = root;
    while (*p) {
        while (*p == '/')
            p++;
        const char *start = p;
        while (*p && *p != '/')
            p++;
        size_t len = (size_t)(p - start);
        if (!len)
            break;
        if ((len == 1 && start[0] == '.') || (len == 2 && start[0] == '.' && start[1] == '.'))
            return fail(e, n, "download_root must not contain . or .. segments");
    }
    return 0;
}

/* Joins an absolute root and a relative file path. The final component is the
 * file name; every parent directory is created before the transfer starts. */
static int join_path(const char *root, const char *path, char *final, char *dir, char *e, size_t n)
{
    if (!path || !*path)
        return fail(e, n, "Download path is empty");
    if (path[0] == '/')
        return fail(e, n, "Download path must be relative");
    size_t rlen = strlen(root);
    while (rlen > 1 && root[rlen - 1] == '/')
        rlen--;
    if (rlen + 1 >= SP_PATH)
        return fail(e, n, "Download path too long");
    memcpy(final, root, rlen);
    final[rlen] = 0;
    if (rlen == 0 || final[rlen - 1] != '/') {
        final[rlen] = '/';
        final[rlen + 1] = 0;
    }
    const char *p = path;
    for (;;) {
        const char *start = p;
        while (*p && *p != '/')
            p++;
        size_t len = (size_t)(p - start);
        if (!len)
            return fail(e, n, "Download path has an empty segment");
        if ((len == 1 && start[0] == '.') || (len == 2 && start[0] == '.' && start[1] == '.'))
            return fail(e, n, "Download path must not contain . or .. segments");
        char segment[512];
        if (sanitize_segment(start, len, segment, sizeof(segment), e, n) ||
            append(final, SP_PATH, segment, e, n))
            return -1;
        if (!*p)
            break;
        if (append(final, SP_PATH, "/", e, n))
            return -1;
        p++;
        if (!*p)
            return fail(e, n, "Download path must not end with /");
    }
    char *slash = strrchr(final, '/');
    size_t dlen = slash ? (size_t)(slash - final) : 0;
    if (!dlen)
        dlen = 1;
    memcpy(dir, final, dlen);
    dir[dlen] = 0;
    return mkdirs(dir) ? fail(e, n, "Cannot create download directory") : 0;
}

static int exists_policy(json_object *options, bool has_path, ExistsPolicy *policy, char *e,
                         size_t n)
{
    json_object *value = jg(options, "on_exists");
    const char *name = NULL;
    if (value && !json_object_is_type(value, json_type_null)) {
        if (!json_object_is_type(value, json_type_string))
            return fail(e, n, "on_exists must be a string");
        name = json_object_get_string(value);
    }
    if (!name)
        name = has_path ? "skip" : "overwrite";
    if (!strcmp(name, "skip"))
        *policy = EXISTS_SKIP;
    else if (!strcmp(name, "overwrite"))
        *policy = EXISTS_OVERWRITE;
    else if (!strcmp(name, "unique"))
        *policy = EXISTS_UNIQUE;
    else
        return fail(e, n, "Unknown on_exists policy");
    return 0;
}

static int existing_result(const DownloadRequest *request, DownloadFile *result, const char *path,
                           char *e, size_t n)
{
    char hash[65];
    struct stat st;
    if (sha256_file(path, hash) || stat(path, &st))
        return fail(e, n, "Cannot inspect existing download");
    int64_t id = request->record ? request->record(request->record_context, request->url, path, hash,
                                                   (int64_t)st.st_size)
                                 : 0;
    if (id < 0)
        return fail(e, n, "Cannot register downloaded file");
    result->id = id;
    result->bytes = (int64_t)st.st_size;
    snprintf(result->path, sizeof(result->path), "%s", path);
    snprintf(result->sha256, sizeof(result->sha256), "%s", hash);
    return 0;
}

static int unique_path(const char *requested, char *chosen, char *e, size_t n)
{
    if (access(requested, F_OK))
        return snprintf(chosen, SP_PATH, "%s", requested) >= SP_PATH
                   ? fail(e, n, "Download path too long")
                   : 0;
    const char *slash = strrchr(requested, '/');
    size_t dirlen = slash ? (size_t)(slash - requested) : 0;
    const char *base = slash ? slash + 1 : requested;
    const char *dot = strrchr(base, '.');
    size_t stem = dot && dot != base ? (size_t)(dot - base) : strlen(base);
    const char *ext = stem < strlen(base) ? base + stem : "";
    for (int i = 1; i < 10000; i++) {
        int written = snprintf(chosen, SP_PATH, "%.*s/%.*s (%d)%s", (int)dirlen, requested,
                               (int)stem, base, i, ext);
        if (written < 0 || written >= SP_PATH)
            return fail(e, n, "Download path too long");
        if (access(chosen, F_OK))
            return 0;
    }
    return fail(e, n, "Too many files with the same name");
}

DownloadClient *download_create(const char *directory, char *e, size_t n)
{
    DownloadClient *client = calloc(1, sizeof(*client));
    if (!client) {
        fail(e, n, "Cannot allocate downloader");
        return NULL;
    }
    if (!directory || strlen(directory) >= sizeof(client->directory) || mkdirs(directory)) {
        fail(e, n, "Cannot create download directory");
        free(client);
        return NULL;
    }
    strcpy(client->directory, directory);
    pthread_mutex_init(&client->mutex, NULL);
    return client;
}

void download_destroy(DownloadClient *client)
{
    if (client) {
        pthread_mutex_destroy(&client->mutex);
        free(client);
    }
}

int download_save(DownloadClient *client, const DownloadRequest *request, DownloadFile *result,
                  char *e, size_t n)
{
    if (!client || !request || !result || !request->url || request->group_id < 0)
        return fail(e, n, "Invalid download request");
    memset(result, 0, sizeof(*result));
    json_object *options = request->options;
    json_object *root_value = jg(options, "download_root");
    const char *root = client->directory;
    if (root_value) {
        if (!json_object_is_type(root_value, json_type_string) ||
            absolute_root(json_object_get_string(root_value), e, n))
            return -1;
        root = json_object_get_string(root_value);
    }
    json_object *path_value = jg(options, "path");
    bool has_path = path_value != NULL;
    ExistsPolicy policy = EXISTS_OVERWRITE;
    char final[SP_PATH], dir[SP_PATH];
    if (exists_policy(options, has_path, &policy, e, n))
        return -1;
    if (has_path) {
        if (!json_object_is_type(path_value, json_type_string))
            return fail(e, n, "Download path must be a string");
        if (join_path(root, json_object_get_string(path_value), final, dir, e, n))
            return -1;
        /* Re-runs skip a complete destination before spending a request. */
        if (policy == EXISTS_SKIP && access(final, F_OK) == 0) {
            int rc = existing_result(request, result, final, e, n);
            if (!rc)
                result->skipped = true;
            return rc;
        }
    } else {
        if (mkdirs(root))
            return fail(e, n, "Cannot create download directory");
        snprintf(dir, sizeof(dir), "%s", root);
    }
    char temp[SP_PATH];
    /* Keep the temporary file beside the target so rename stays atomic on the
     * same filesystem, then publish only after fsync and hashing. */
    if (snprintf(temp, sizeof(temp), "%s/.part-XXXXXX", dir) >= (int)sizeof(temp))
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
    int rc = download_fetch(request->options, request->url, f, request->cancelled,
                            request->cancel_context, request->session, &response, e, n);
    if (fflush(f) || ferror(f) || fsync(fd))
        rc = fail(e, n, "Cannot persist download");
    if (fclose(f))
        rc = fail(e, n, "Cannot close download");
    if (rc) {
        unlink(temp);
        download_response_free(&response);
        return -1;
    }
    char hash[65];
    if (sha256_file(temp, hash)) {
        unlink(temp);
        download_response_free(&response);
        return fail(e, n, "Cannot hash download");
    }
    if (!has_path) {
        if (snprintf(final, sizeof(final), "%s/%lld-%s.bin", root, (long long)request->group_id,
                     hash) >= (int)sizeof(final)) {
            unlink(temp);
            download_response_free(&response);
            return fail(e, n, "Download path too long");
        }
    }
    /* Publication, registration and rollback form one ownership boundary.
     * Downloads run concurrently, but no rollback may remove a peer's file. */
    pthread_mutex_lock(&client->mutex);
    if (policy == EXISTS_SKIP && access(final, F_OK) == 0) {
        unlink(temp);
        download_response_free(&response);
        rc = existing_result(request, result, final, e, n);
        if (!rc)
            result->skipped = true;
        pthread_mutex_unlock(&client->mutex);
        return rc;
    }
    char chosen[SP_PATH];
    if (policy == EXISTS_UNIQUE) {
        if (unique_path(final, chosen, e, n)) {
            unlink(temp);
            download_response_free(&response);
            pthread_mutex_unlock(&client->mutex);
            return -1;
        }
        snprintf(final, sizeof(final), "%s", chosen);
    }
    bool existed = access(final, F_OK) == 0;
    if (rename(temp, final)) {
        unlink(temp);
        download_response_free(&response);
        pthread_mutex_unlock(&client->mutex);
        return fail(e, n, "Cannot finalize download");
    }
    int64_t id = request->record ? request->record(request->record_context, request->url, final, hash,
                                                   (int64_t)response.size)
                                 : 0;
    result->bytes = (int64_t)response.size;
    result->http_status = response.status;
    download_response_free(&response);
    if (id < 0) {
        if (!existed)
            unlink(final);
        pthread_mutex_unlock(&client->mutex);
        return fail(e, n, "Cannot register downloaded file");
    }
    pthread_mutex_unlock(&client->mutex);
    result->id = id;
    strcpy(result->path, final);
    strcpy(result->sha256, hash);
    return 0;
}
