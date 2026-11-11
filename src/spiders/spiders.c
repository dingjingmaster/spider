#include "spiders.h"
#include "workflow/workflow.h"
int spiders_register(CrawlerRegistry *registry, char *e, size_t n)
{
    /* Workflow is an available engine, not a runnable site task by itself.
     * Add real site registrations and crawler_add() calls here. */
    return workflow_register(registry, e, n);
}
