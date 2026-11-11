#include "spiders/spiders.h"
#include "scheduler/scheduler.h"
#include "web/web.h"
#include <curl/curl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>

static volatile sig_atomic_t interrupted;


static void stop_signal(int sig)
{
    (void)sig;
    interrupted = 1;
}

static int number(const char *s, int low, int high)
{
    errno = 0;
    char *end;
    long v = strtol(s, &end, 10);
    return errno || *end || v < low || v > high ? -1 : (int)v;
}

static int copy_arg(char *dest, size_t cap, const char *s)
{
    if (strlen(s) >= cap)
        return -1;
    strcpy(dest, s);
    return 0;
}

static void help(void)
{
    puts("Spider 0.1.0 - C / SQLite crawler\nUsage: spider [--host 127.0.0.1] [--port 8080] "
         "[--workers 4]\n              [--data-dir ./data] [--ui-dir PATH] [--token "
         "TOKEN]\nNon-loopback binding requires --token. No browser runtime required.");
}

int main(int argc, char **argv)
{
    struct {
        char token[256], data_dir[SP_PATH], ui_dir[SP_PATH], listen[128];
    } app = {0};

    char error[SP_ERROR] = {0}, data[SP_PATH] = "data", host[80] = "127.0.0.1", ui[SP_PATH] = "";
    int port = 8080, workers = 4;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            help();
            return 0;
        }
        if (i + 1 >= argc) {
            fprintf(stderr, "Missing option value\n");
            return 2;
        }
        const char *key = argv[i], *value = argv[++i];
        int rc = 0;
        if (!strcmp(key, "--port")) {
            port = number(value, 1, 65535);
            rc = port < 0;
        } else if (!strcmp(key, "--workers")) {
            workers = number(value, 1, SP_MAX_WORKERS);
            rc = workers < 0;
        } else if (!strcmp(key, "--data-dir")) {
            rc = copy_arg(data, sizeof(data), value);
        } else if (!strcmp(key, "--host")) {
            rc = copy_arg(host, sizeof(host), value);
        } else if (!strcmp(key, "--ui-dir")) {
            rc = copy_arg(ui, sizeof(ui), value);
        } else if (!strcmp(key, "--token")) {
            rc = copy_arg(app.token, sizeof(app.token), value);
        } else {
            rc = -1;
        }

        if (rc) {
            fprintf(stderr, "Invalid option: %s\n", key);
            return 2;
        }
    }

    if (strcmp(host, "127.0.0.1") && strcmp(host, "localhost") && strcmp(host, "[::1]") &&
        !app.token[0]) {
        fprintf(stderr, "Non-loopback binding requires --token\n");
        return 2;
    }

    if (strspn(host, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.:-[]") !=
        strlen(host)) {
        fprintf(stderr, "Invalid host\n");
        return 2;
    }

    if (!ui[0]) {
        char exe[SP_PATH];
        ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (len > 0) {
            exe[len] = 0;
            char *slash = strrchr(exe, '/');
            if (slash) {
                *slash = 0;
            }
            if (snprintf(ui, sizeof(ui), "%s/ui", exe) >= (int)sizeof(ui)) {
                return 2;
            }
            if (access(ui, R_OK)) {
                if (snprintf(ui, sizeof(ui), "%s/../share/spider/ui", exe) >= (int)sizeof(ui)) {
                    return 2;
                }
            }
        } else {
            strcpy(ui, "src/web/ui");
        }
    }
    if (!realpath(ui, app.ui_dir)) {
        fprintf(stderr, "Cannot locate UI directory: %s\n", ui);
        return 2;
    }
    umask(0077);
    if (mkdirs(data) || !realpath(data, app.data_dir)) {
        fprintf(stderr, "Cannot create data directory\n");
        return 1;
    }
    char path[SP_PATH];
    if (snprintf(path, sizeof(path), "%s/.lock", app.data_dir) >= (int)sizeof(path)) {
        return 1;
    }

    int lock = open(path, O_CREAT | O_RDWR, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB)) {
        fprintf(stderr, "Data directory is already in use\n");
        if (lock >= 0) {
            close(lock);
        }
        return 1;
    }

    if (snprintf(path, sizeof(path), "%s/downloads", app.data_dir) >= (int)sizeof(path) ||
        mkdirs(path)) {
        close(lock);
        return 1;
    }

    DIR *dir = opendir(path);
    struct dirent *entry;
    while (dir && (entry = readdir(dir))) {
        if (!strncmp(entry->d_name, ".part-", 6)) {
            char part[SP_PATH];
            if (snprintf(part, sizeof(part), "%s/%s", path, entry->d_name) < (int)sizeof(part)) {
                unlink(part);
            }
        }
    }

    if (dir) {
        closedir(dir);
    }
    if (snprintf(path, sizeof(path), "%s/spider.db", app.data_dir) >= (int)sizeof(path)) {
        close(lock);
        return 1;
    }
    if (curl_global_init(CURL_GLOBAL_DEFAULT)) {
        close(lock);
        return 1;
    }

    Store *store = store_create(path, error, sizeof(error));
    DownloadClient *downloads = NULL;
    CrawlerRegistry *registry = NULL;
    Status *status = NULL;
    Scheduler *scheduler = NULL;
    Web *web = NULL;
    int result = 1;
    if (!store) {
        goto done;
    }

    if (snprintf(path, sizeof(path), "%s/downloads", app.data_dir) >= (int)sizeof(path)) {
        goto done;
    }

    downloads = download_create(path, error, sizeof(error));
    status = status_create(store);
    registry = crawler_create();
    if (!downloads || !status || !registry || spiders_register(registry, error, sizeof(error)) ||
        crawler_sync(registry, store, error, sizeof(error))) {
        goto done;
    }

    scheduler = scheduler_create(store, registry, downloads, status);
    if (!scheduler || scheduler_start(scheduler, workers, error, sizeof(error))) {
        goto done;
    }

    snprintf(app.listen, sizeof(app.listen), "%s:%d", host, port);
    struct sigaction sa = {0};
    sa.sa_handler = stop_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    WebConfig config = {.listen = app.listen, .ui_directory = app.ui_dir, .token = app.token};

    web = web_start(&config, status, error, sizeof(error));
    if (!web) {
        goto done;
    }

    fprintf(stderr, "Spider listening on http://%s | data: %s\n", app.listen, app.data_dir);
    while (!interrupted) {
        sleep_ms(100);
    }

    result = 0;

done:
    if (result) {
        fprintf(stderr, "%s\n", *error ? error : "Cannot initialize crawler modules");
    }
    scheduler_stop(scheduler);
    web_stop(web);
    scheduler_destroy(scheduler);
    crawler_destroy(registry);
    status_destroy(status);
    download_destroy(downloads);
    store_destroy(store);
    curl_global_cleanup();
    close(lock);

    return result;
}
