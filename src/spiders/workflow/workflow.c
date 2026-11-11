#include "workflow.h"
#include "private/engine.h"
#include "private/preview.h"
#include "rule/rule.h"
#include <stdlib.h>
#include <string.h>
static int run(CrawlerContext *context, char *e, size_t n)
{
    if (wf_initialize_run(context->store, context->run_id, e, n))
        return -1;
    while (!crawler_cancelled(context)) {
        json_object *job = wf_claim_run(context->store, context->run_id, e, n);
        if (!job) {
            if (e && *e)
                return -1;
            sleep_ms(25);
            continue;
        }
        status_progress(context->status, context->run_id, js(job, "step_id", ""), 0, 0);
        workflow_execute(context, job);
        json_object_put(job);
    }
    json_object *result = store_run(context->store, context->run_id);
    int rc = 0;
    if (!result)
        rc = fail(e, n, "Crawler run disappeared");
    else if (!strcmp(js(result, "status", ""), "failed"))
        rc = fail(e, n, "%s", js(result, "error", "Workflow failed"));
    else if (!strcmp(js(result, "status", ""), "partial"))
        rc = 1;
    json_object_put(result);
    return rc;
}
static json_object *preview(void *data, json_object *payload, bool single, const atomic_bool *stop,
                            char *e, size_t n)
{
    if (single)
        return workflow_preview_step(data, stop, payload, e, n);
    CrawlerContext context = {.stopping = stop};
    return workflow_preview(&context, payload, e, n);
}
static int release(void *data, const char *id, char *e, size_t n)
{
    return preview_pool_release(data, id, e, n);
}
static void destroy(void *data)
{
    preview_pool_destroy(data);
}
int workflow_register(CrawlerRegistry *registry, char *e, size_t n)
{
    PreviewPool *previews = preview_pool_create();
    if (!previews)
        return fail(e, n, "Cannot create workflow preview pool");
    CrawlerDefinition definition = {.id = "workflow",
                                    .name = "JSON workflow",
                                    .validate = rule_validate,
                                    .run = run,
                                    .preview = preview,
                                    .release_preview = release,
                                    .destroy = destroy,
                                    .userdata = previews};
    int rc = crawler_register(registry, &definition, e, n);
    if (rc)
        preview_pool_destroy(previews);
    return rc;
}
