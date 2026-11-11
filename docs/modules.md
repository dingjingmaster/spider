# 模块和爬虫接入规范

所有源码位于 `src/`。每个模块只提供一对同名公共 `.h/.c`；复杂实现放在模块的 `private/`，其他产品模块不引用其私有头文件。所有模块由 `main/main.c` 初始化和按生命周期销毁。

## 模块职责

| 模块 | 公共接口 | 职责 |
|---|---|---|
| crawler | crawler.h / crawler.c | 实现注册、代码任务登记与同步、配置校验、每次运行上下文、协作取消 |
| download | download.h / download.c | 网页/接口请求、Cookie 导入导出、流式文件下载、进度、临时文件与发布回滚 |
| parser | parser.h / parser.c | HTML/CSS、JSON Pointer、字段类型转换；不联网、不操作数据库 |
| storage | storage.h / storage.c | 配置、版本快照、运行、文件、会话、结果和工作流检查点；内部持有 SQLite 与锁 |
| scheduler | scheduler.h / scheduler.c | 定时控制线程、固定工作线程池、完整运行领取、启动/取消、同一爬虫防重 |
| status | status.h / status.c | 持久生命周期与计数的查询，内存中的当前阶段/传输进度投影 |
| web | web.h / web.c | 只读状态 API 与看板；只依赖 status 模块，不拼 SQL |
| rule | rule.h / rule.c | JSON 工作流校验、模板、转换及旧定义升级；私有辅助实现不向外暴露 |
| spiders | spiders.h / spiders.c | 内置爬虫统一注册入口；各网站实现放在子目录 |

`common` 和 `model` 提供小型通用工具和数据类型。`browser` 是未启用的能力接口，不具备 JavaScript 执行能力。

`download` 不依赖 `storage`、`scheduler` 或 `web`。文件发布后的数据库登记通过 `DownloadRecord` 回调完成，发布和失败回滚由下载模块的锁保护。`parser` 只处理输入内容。只有 `storage` 的实现能够访问 SQLite 和执行 SQL，`Store` 在公共头文件中是不透明句柄。网页只有状态查询；启动、周期和任务参数由代码决定。

## 并发和运行身份

调度单位是一次完整的爬虫运行。默认 4 个工作线程，`--workers 1..16` 可调整；不同爬虫配置 ID 的运行可并发，同一配置 ID 的执行入口和所有请求始终在同一个工作线程内串行完成。爬虫实现不得自行创建线程或并发传输。

`implementation` 是编译注册的实现类型，例如 `workflow`；`code_id` 是代码登记的稳定任务标识；数据库 `spider_id` 通过 code_spiders 映射到该任务，`run_id` 是一次运行。一个实现类型可由代码登记多个任务。同一域名的不同配置仍然可能并发访问；本版本没有跨爬虫域名限速。

一个爬虫最多有一个排队或运行中的任务。到期时已有任务则合并本次触发，更新下次触发时间，不积压重复任务。取消会立即持久化状态并通知执行逻辑退出，调度器保留该爬虫的占用资格直到执行回调和资源清理全部结束；内部调度接口在此期间拒绝再次启动；产品 Web 不提供运行控制。网络、解析、重试等待占用该爬虫的工作线程；网络和文件 IO 不持有调度器或数据库锁。

服务退出时先通知并等待调度工作线程，再停止 Web 并等待请求处理结束，然后销毁注册表、状态、下载和存储模块。运行内不要修改 `context->config`，它是提交运行时固定的规则快照。

## 原生 C 爬虫

新增 `src/spiders/my_site/my_site.h` 和 `my_site.c`，对外只提供注册函数。下面是“请求页面、解析、保存记录”的最小实现，文件下载可以按业务需要加入同一循环。

```c
/* my_site.h */
#ifndef MY_SITE_H
#define MY_SITE_H
#include "crawler/crawler.h"
int my_site_register(CrawlerRegistry *, char *error, size_t cap);
#endif
```

```c
/* my_site.c */
#include "my_site.h"
#include "parser/parser.h"
#include "rule/rule.h"
#include <stdlib.h>

static int run(CrawlerContext *context, char *error, size_t cap)
{
    HttpResponse response = {0};
    json_object *items = NULL;
    char *next = NULL;
    int rc = crawler_fetch(context, context->config,
                           js(context->config, "url", ""), &response, error, cap);
    if (rc) goto done;
    rc = extract_document(context->config, response.body, response.url,
                          &items, &next, error, cap);
    if (rc) goto done;
    for (size_t i = 0; i < json_object_array_length(items); i++) {
        if (crawler_cancelled(context)) { rc = -1; break; }
        json_object *item = json_object_array_get_idx(items, i);
        const char *key = js(item, "url", "");
        rc = store_emit(context->store, context->run_id, key,
                        response.url, item, error, cap);
        if (rc) break;
    }
done:
    free(next);
    json_object_put(items);
    download_response_free(&response);
    return rc;
}

int my_site_register(CrawlerRegistry *registry, char *error, size_t cap)
{
    CrawlerDefinition definition = {
        .id = "my-site", .name = "My site",
        .validate = request_validate, .run = run
    };
    if (crawler_register(registry, &definition, error, cap)) return -1;
    json_object *config = json_tokener_parse(
        "{\"name\":\"网站资源\",\"url\":\"https://example.com/resources\","
        "\"format\":\"html\",\"row_selector\":\"a.download\","
        "\"fields\":[{\"name\":\"title\",\"selector\":\"\"},"
        "{\"name\":\"url\",\"selector\":\"\",\"attribute\":\"href\",\"download\":true,\"required\":true}],"
        "\"interval_seconds\":3600,\"enabled\":true}");
    if (!config) return fail(error, cap, "Invalid site configuration");
    int rc = crawler_add(registry, "my-site", "my-site", config, error, cap);
    json_object_put(config);
    return rc;
}
```

在 `spiders/spiders.c` 包含该公共头文件并调用注册函数，将 `.c` 加入 `src/CMakeLists.txt` 的 `spider_core` 源文件列表后重新编译。注册必须在 `scheduler_start` 前完成；注册表在启动后冻结。类型 ID 不可重复，注册表复制 ID/name 并在销毁时调用可选的 `destroy(userdata)`。注册失败时 userdata 仍由调用者负责释放。

配置写在上述 C 实现中，也可以由代码构造 JSON 对象；对应的逻辑内容如下（不通过网页导入）：

```json
{
  "implementation": "my-site",
  "name": "网站资源",
  "url": "https://example.com/resources",
  "format": "html",
  "row_selector": "a.download",
  "fields": [
    {"name": "title", "selector": ""},
    {"name": "url", "selector": "", "attribute": "href", "download": true, "required": true}
  ],
  "interval_seconds": 3600,
  "enabled": true
}
```

`download: true` 只把链接转换为绝对地址，不会自动下载。需要实际下载时，在循环里调用 `crawler_save_file(context, options, url, &file, error, cap)`。它组合下载模块和持久化登记，返回 `DownloadFile`，同一次运行的已登记且仍可读 URL 可复用。

下载参数支持可选的 `download_root`（绝对路径，缺失时使用数据目录的 `downloads/`）、`path`（相对路径，含文件名，父目录会自动创建）和 `on_exists`（`skip`、`overwrite`、`unique`）。未提供 `path` 时保持 `{运行ID}-{SHA-256}.bin` 的旧命名，默认覆盖；提供 `path` 时默认跳过已存在的文件。`path` 必须是相对路径，不允许 `.`、`..` 和空路径段；控制字符和反斜杠会替换为 `_`。爬虫顶层配置里的 `download_root`、`on_exists` 会被 `crawler_save_file` 自动继承，单次调用的 options 优先。单独使用下载模块时，`download_save` 的 `record` 回调接收文件信息，不接收数据库句柄。

实现类型与任务分开注册：`crawler_register` 定义如何执行，`crawler_add` 定义一个具体任务。`crawler_add` 复制配置并补充 implementation，调用对应校验器；代码 ID 不允许重复。main 在调度器启动前调用 `crawler_sync`，事务同步当前任务目录。相同代码 ID 保留数据库 ID/历史；配置未变不追加版本、不重置 next_run；移除任务后停用并取消未完成运行，重新登记同一代码 ID 可恢复身份。

Web 只读取当前代码目录的状态；没有新建、编辑、导入、试采集、运行或停止操作，也不返回代码参数和规则快照。目前内置 `spiders/workflow/` 为可复用引擎，尚未登记实际网站任务；上述示例须开发者实际加入编译并替换域名和选择器。未登记任务时状态页显示空目录。

如需在代码中使用已有 JSON 工作流，先调用 `workflow_register`（默认 `spiders_register` 已调用），再用 `crawler_add(registry, "stable-site-id", "workflow", workflow_json, ...)` 登记固定任务。旧工作流协议仅用于引擎和测试驱动，产品没有工作流编辑或调试 API。

## 通用下载目录

`storage` 提供跨运行的通用下载目录，音乐、小说等爬虫共用，不按站点建表：

- `download_sources`：条目/合集级元数据，唯一键 `(spider_code, source, media_type, identifier)`。
- `download_items`：每个可下载文件一行，保存 `download_url`、`status`、`attempts`、`target_path`、本地哈希等；`(source_id, logical_key)` 唯一，`download_url` 全局唯一。
- `music_tracks` / `novel_files`：类型明细，和 `download_items.id` 一对一。
- `music_downloads` / `novel_downloads`：按类型拼好的视图，查询时像单表一样使用。

站点差异放在 `download_sources.metadata_json/extra_json`、`download_items.options_json/extra_json` 这类 JSON 字段里，不为每个站点新增列。下载参数（`headers`、`method`、`body` 等）放 `options_json`，不要把密钥写进去；需要认证时用 `auth_key` 指向代码内配置。多段资源用 `group_key`、`part_index`、`part_count` 表达；同一内容跨站点去重用 `content_key` / `remote_sha1`。

接口：`store_download_scan`（一个事务写入 source 和 items）、`store_download_queue`（取待下载行）、`store_download_start` / `store_download_finish`（状态流转）、`store_download_decide`（人工改 decision 并置 curated）、`store_download_summary`。SQL 仍只存在于 storage 内部。

## 恢复、所有权和状态

`CrawlerContext` 和配置快照在回调期间借用，禁止保存到全局变量或在返回后使用。每次运行拥有独立 Cookie 会话；`crawler_fetch` 和 `crawler_save_file` 在请求后通过存储模块保存 Cookie。响应和返回的 JSON 由调用者释放，Cookie export/import 不访问数据库。

工作流实现逐运行初始化/领取持久化步骤，保存过的步骤不重跑；`foreach`、`parallel` 保留独立上下文和汇合结果，分支在同一线程顺序推进。POST 中断后的不确定结果继续遵循显式重试授权规则。

原生 C 爬虫的函数调用栈不能持久化；服务重启后从运行入口重新进入。需要使用已保存结果、文件记录或业务检查点避免重复副作用。`store_emit` 的 stable key 按一次运行幂等，并与结果计数在同一事务提交；不表示跨运行增量去重。对外部网站的 POST 也不会自动获得幂等保证。

生命周期状态以数据库为准。`status_progress` 更新当前阶段和传输字节数，Web 查询到的 `execution_active` 表示本进程仍在执行/清理该运行。没有总长度时不显示虚假的百分比。执行结束后清理内存进度，历史状态、步骤、结果和文件记录仍保留在 SQLite 中。

## 验证边界

单元与真实本地 HTTP 测试覆盖同一爬虫串行、跨爬虫并发、取消退出防重、任务目录同步和事务回滚、重启后的稳定身份、成功/失败状态以及 Web 只读边界。旧引擎的变更/预览控制只存在于 tests/support 的独立测试程序，产品 Web 不链接该控制实现。测试不访问业务网站或用户数据库。
