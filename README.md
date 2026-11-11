# Spider

C 实现的单机爬虫服务：在 `src/spiders/` 编写并注册网站采集逻辑，由调度器定时运行，将结果保存到 SQLite、文件保存到本地磁盘。Web 页面只显示已注册爬虫及其运行状态、成功/失败历史和错误原因。

## 构建和启动

需要 Linux/POSIX、C11 编译器、CMake >= 3.16、pkg-config，以及开发库 libcurl >= 7.85、SQLite >= 3.35、json-c >= 0.15、OpenSSL >= 1.1.1。CivetWeb 1.16 和 Lexbor 3.0.0 源码随项目提供，构建时不下载依赖。

```sh
sudo apt install build-essential cmake pkg-config libcurl4-openssl-dev libsqlite3-dev libjson-c-dev libssl-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/spider --data-dir ./data --port 8080
```

打开 **http://127.0.0.1:8080** 查看状态看板。页面自动刷新，显示爬虫列表、执行周期、最近状态、成功/失败次数、运行历史、时间、结果数、当前进度和错误原因。没有代码任务时显示空状态；通用引擎的注册不等于已经接入实际网站。

```sh
./build/spider --help
./build/spider --host 0.0.0.0 --port 8080 --workers 4 --data-dir ./data --token YOUR_TOKEN
```

非本机绑定必须提供 token；在页面“连接设置”中填写同一口令，仅保存于当前浏览器会话。API 使用 `Authorization: Bearer TOKEN`。内置监听器为 HTTP，远程访问通过 HTTPS 反向代理。程序继承 libcurl 的代理环境设置，本地测试可使用 `NO_PROXY=127.0.0.1,localhost`。

## 添加爬虫

1. 新建 `src/spiders/网站名/网站名.h` 和 `.c`，实现 `CrawlerDefinition.run`，组合下载、解析、持久化接口。
2. 注册实现类型，再在代码中调用 `crawler_add(registry, stable_code_id, implementation, config, ...)` 登记任务的名称、参数和执行周期。
3. 在 `src/spiders/spiders.c` 调用该网站的注册函数，把 `.c` 加入 `src/CMakeLists.txt` 后重新编译。

完整示例、所有权和恢复约定见 [模块与爬虫接入规范](docs/modules.md)。当前仓库保留 `spiders/workflow/` 通用工作流引擎，尚未登记具体业务网站任务。它也可以由代码提交固定工作流任务，协议参考 [工作流引擎说明](docs/workflows.md)。网页不提供爬虫创建、编辑、导入、试采集、手动运行或停止操作。

## 调度

默认 4 个工作线程，`--workers 1..16` 可调整。不同爬虫任务并发；同一任务的执行入口、请求、解析、下载和重试在同一个线程中串行完成。一次完整运行占用一个线程，代码不得自行创建并发采集线程。

`interval_seconds` 为执行间隔，0 表示不自动定时，`enabled` 控制定时计划；这些值来自代码配置。到期时已有任务则合并触发，不积压重复运行。爬虫身份由稳定代码 ID 映射为数据库 ID；同一网站的不同任务仍可能同时访问，不具备全局域名限速。

启动时在一个事务中同步代码目录：相同代码 ID 保留数据库 ID 和历史；未变化配置保留版本和下次执行时间；变化配置生成新版本。已从代码移除或旧网页创建的任务停用并取消未完成运行，历史数据保留。移除后重新登记相同代码 ID 会恢复原有身份。

Ctrl+C/SIGTERM 协作取消并等待线程退出。工作流按已提交步骤恢复；原生 C 爬虫重启后从运行入口重新进入，需自行利用持久化记录避免重复副作用。非幂等 POST 的恢复边界见工作流说明。

## 模块

```text
src/
  main/          初始化、CLI、信号和单实例锁
  crawler/       爬虫接口、实现注册、代码任务目录、独立运行上下文
  spiders/       各网站实现与统一注册入口
    workflow/    可供代码复用的 JSON 工作流引擎
  scheduler/     定时线程、固定工作线程池、完整运行调度
  download/      HTTP、Cookie、流式下载、文件发布与进度
  parser/        HTML/CSS、JSON Pointer、字段类型转换
  storage/       SQLite、代码 ID 映射、快照、检查点、结果和事务
  status/        当前代码任务的只读状态、进度和历史查询
  web/           只读 HTTP API
    ui/          状态看板
  rule/          工作流校验、模板与旧格式升级
  common/ model/ 通用工具和类型
  tests/         单元、真实本地 HTTP 与页面逻辑测试
    support/     仅用于测试的引擎控制程序，不链接进产品
  third_party/   CivetWeb、Lexbor
```

每个功能模块通过一对公共 `.h/.c` 提供功能，辅助实现位于 `private/`。SQLite/SQL 仅在 storage 内部，下载模块通过回调登记文件，Web 只依赖 status 模块。

## 数据保存

默认数据目录为启动位置的 `./data`，可用 `--data-dir` 指定：

- `spider.db`：代码任务目录、历史配置/版本、运行、检查点、HTTP 尝试、结果、文件元数据和事件。
- `downloads/`：默认按运行 ID＋SHA-256 命名为 `.bin`。爬虫可在下载参数中给出 `download_root`（必须是绝对路径）和 `path`（相对 `download_root` 的路径，含文件名），模块会递归创建父目录并按该路径保存；`on_exists` 支持 `skip`（有 `path` 时默认）、`overwrite`（无 `path` 时的默认）和 `unique`。爬虫顶层配置里的 `download_root`、`on_exists` 会被 `crawler_save_file` 自动继承。

SQLite 使用 WAL、外键和短事务。核心 `user_version` 为 3，新增的 `code_spiders` 是启动时自动创建的辅助映射表。旧定义迁移规则保留；同步当前代码目录后，未注册的历史任务不再参与调度。更新前应停止服务并备份数据目录。

结果记录与步骤推进在事务中保存，网络不占用数据库事务。附件先写临时文件、fsync、重命名，再通过登记回调保存元数据；失败回滚不会删除其他下载已登记的同内容文件。同一运行内登记过且仍可读的 URL 可复用，不等于跨运行资源去重。文件中断后重新下载，目前没有文件字节级续传、磁盘配额或自动历史清理。

## 只读 API

配置 token 后所有 API 要求认证。只接受 GET；POST、PUT、DELETE 等请求返回 405，未认证请求先返回 401。响应不提供规则快照、Cookie 或代码参数。

| 路径 | 用途 |
|---|---|
| `/api/health` | 版本和只读模式 |
| `/api/spiders` | 当前代码任务、最近状态、周期与成功/失败计数 |
| `/api/spiders/{id}` | 当前代码任务的状态摘要 |
| `/api/runs?spider_id={id}` | 当前代码任务最近 200 次运行 |
| `/api/runs/{id}` | 执行状态、时间、错误、结果数和进度 |
| `/api/runs/{id}/events` | 最近 200 条运行事件 |

历史数据库中已移除的任务继续保留，但不出现在当前状态看板中。

## 验证

```sh
ctest --test-dir build --output-on-failure -j4
cmake -S . -B build-asan -DSPIDER_SANITIZE=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-asan -j
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-asan --output-on-failure -j4
```

测试使用临时 SQLite 和本地 HTTP 服务，不访问实际业务网站。旧工作流的网络、预览、取消和恢复回归由 tests 下的独立 `spider_engine_fixture` 驱动，其写控制接口不在产品 `spider` 中编译。`spider_status_fixture` 注册真实 C 测试爬虫，使用产品 Web 验证定时成功/失败、稳定 ID、旧任务停用和只读边界。Python/Node 只用于测试，不是服务运行依赖。
