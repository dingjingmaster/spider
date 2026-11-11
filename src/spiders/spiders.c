#include "spiders.h"
#include "workflow/workflow.h"
int spiders_register(CrawlerRegistry *registry, char *e, size_t n)
{
    return workflow_register(registry, e, n);
}
