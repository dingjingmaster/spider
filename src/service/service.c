#include "service.h"
#include "engine/engine.h"
#include "scheduler/scheduler.h"
static void *worker(void *ctx)
{
    App *app = ctx;
    while (!atomic_load(&app->stopping)) {
        scheduler_tick(&app->store);
        json_object *run = store_claim_run(&app->store);
        if (run) {
            engine_run(app, run);
            json_object_put(run);
        } else
            sleep_ms(100);
    }
    return NULL;
}
int service_start(App *app, int workers, char *e, size_t n)
{
    app->worker_count = 0;
    atomic_init(&app->stopping, false);
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
}
