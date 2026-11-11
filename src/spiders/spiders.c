#include "spiders.h"
#include "workflow/workflow.h"
#include "archive_org/archive_org.h"



int spiders_register(CrawlerRegistry *registry, char *e, size_t n)
{
    /* Workflow is an available engine, not a runnable site task by itself.
     * Add real site registrations and crawler_add() calls here. */
    if (workflow_register(registry, e, n)) {
        return -1;
    }

    return archive_org_register(registry, e, n);
}
