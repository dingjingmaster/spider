#include "download/download.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
struct DownloadClient {
    char directory[SP_PATH];
    pthread_mutex_t mutex;
};
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
    char temp[SP_PATH], final[SP_PATH];
    if (snprintf(temp, sizeof(temp), "%s/.part-XXXXXX", client->directory) >= (int)sizeof(temp))
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
    if (snprintf(final, sizeof(final), "%s/%lld-%s.bin", client->directory,
                 (long long)request->group_id, hash) >= (int)sizeof(final)) {
        unlink(temp);
        download_response_free(&response);
        return fail(e, n, "Download path too long");
    }
    /* Publication, registration and rollback form one ownership boundary.
     * Downloads run concurrently, but no rollback may remove a peer's file. */
    pthread_mutex_lock(&client->mutex);
    bool existed = access(final, F_OK) == 0;
    if (rename(temp, final)) {
        unlink(temp);
        download_response_free(&response);
        pthread_mutex_unlock(&client->mutex);
        return fail(e, n, "Cannot finalize download");
    }
    int64_t id = request->record ? request->record(request->record_context, request->url, final,
                                                   hash, (int64_t)response.size)
                                 : 0;
    result->bytes = (int64_t)response.size;
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
