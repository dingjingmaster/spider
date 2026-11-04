#include "service.h"
#include "scheduler/scheduler.h"
#include "storage/workflow.h"
#include "engine/workflow.h"
#include "engine/preview.h"
static void *worker(void *ctx)
{
    App *app = ctx;
    while (!atomic_load(&app->stopping)) {
        scheduler_tick(&app->store);
        char error[SP_ERROR] = {0};
        if (wf_initialize(&app->store, error, sizeof(error))) {
            fprintf(stderr, "%s\n", error);
            sleep_ms(100);
            continue;
        }
        json_object *job = wf_claim(&app->store, error, sizeof(error));
        if (job) {
            workflow_execute(app, job);
            json_object_put(job);
            continue;
        }
        if (*error) {
            fprintf(stderr, "%s\n", error);
            sleep_ms(100);
            continue;
        }
        sleep_ms(100);
    }
    return NULL;
}
int service_start(App *app, int workers, char *e, size_t n)
{
    app->worker_count = 0;
    atomic_init(&app->stopping, false);
    pthread_mutex_init(&app->artifact_mutex, NULL);
    app->sessions = session_pool_create(&app->store);
    if (!app->sessions) {
        pthread_mutex_destroy(&app->artifact_mutex);
        return fail(e, n, "Cannot initialize session pool");
    }
    app->previews = preview_pool_create();
    if (!app->previews) {
        session_pool_destroy(app->sessions);
        app->sessions = NULL;
        pthread_mutex_destroy(&app->artifact_mutex);
        return fail(e, n, "Cannot initialize preview sessions");
    }
    for (int i = 0; i < workers; i++) {
        if (pthread_create(&app->workers[i], NULL, worker, app)) {
            service_stop(app);
            return fail(e, n, "Cannot start worker thread");
        }
        app->worker_count++;
    }
    return 0;
}
void service_stop(App *app)
{
    atomic_store(&app->stopping, true);
    for (int i = 0; i < app->worker_count; i++)
        pthread_join(app->workers[i], NULL);
    app->worker_count = 0;
    preview_pool_destroy(app->previews);
    app->previews = NULL;
    session_pool_destroy(app->sessions);
    app->sessions = NULL;
    pthread_mutex_destroy(&app->artifact_mutex);
}
