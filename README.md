# Spider

使用 C 实现的单机采集工作台：在网页配置工作流、请求与解析步骤，试采集，定时或手动执行，将结构化数据保存到 SQLite，将附件保存到本地磁盘。

## 构建和启动

需要 Linux/POSIX 环境、C11 编译器、CMake >= 3.16、pkg-config，以及开发库：libcurl >= 7.85、SQLite >= 3.35、json-c >= 0.15、OpenSSL >= 1.1.1。CivetWeb 1.16 和 Lexbor 3.0.0 的源码已随项目提供，构建时不下载依赖。

Debian/Ubuntu 可安装：

```sh
sudo apt install build-essential cmake pkg-config libcurl4-openssl-dev libsqlite3-dev libjson-c-dev libssl-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/spider --data-dir ./data --port 8080
```

打开 **http://127.0.0.1:8080**。点击左侧 `＋` 新建爬虫，默认生成 `request → foreach → emit` 工作流。填写 request 步骤的请求地址和提取字段，选择该步骤“试采集”；保存后可立即运行，或者设置执行间隔。网页还支持逐步骤配置、完整 JSON 和规则导入导出。

```sh
./build/spider --help
./build/spider --host 0.0.0.0 --port 8080 --data-dir ./data --token YOUR_TOKEN
```

远程绑定必须提供 token，在网页“连接设置”中填写同一口令。非本机访问应通过 HTTPS 反向代理；程序内置监听器提供 HTTP。token 通过 Authorization 请求头传递，不放 URL。浏览器将口令保存在当前会话的 sessionStorage 中。程序继承 libcurl 的代理环境设置；本地测试使用 `NO_PROXY=127.0.0.1,localhost`。

默认一个工作线程，可通过 `--workers 1..16` 显式增加并发。所有运行及其步骤共享线程池；一个线程一次执行一个步骤，foreach/parallel 等待子分支时不占用线程。增加线程后，同一工作流的独立分支和不同运行可并发执行。`delay_ms` 是请求步骤的等待策略，不是跨爬虫的全局域名限速。一个数据目录只允许一个进程使用。Ctrl+C/SIGTERM 后按步骤恢复；网页“停止运行”会取消该运行。POST 中断后的恢复限制见 [工作流说明](docs/workflows.md#失败与恢复)。

## 规则示例

网页、规则导入和写入/试采集 API 统一使用 `schema_version: 2`。普通页面采集也使用同一工作流：request 提取列表，foreach 展开，emit 保存每条结果。

```json
{
  "schema_version": 2,
  "name": "页面标题",
  "entry": "page",
  "steps": [
    {
      "id": "page", "type": "request",
      "request": {"url": "https://example.com/", "method": "GET"},
      "extract": {"format": "html", "fields": [{"name": "title", "selector": "h1", "required": true}]},
      "next": "each"
    },
    {"id": "each", "type": "foreach", "items": "{{steps.page.items}}", "body": "save"},
    {"id": "save", "type": "emit", "fields": {"title": "{{input.title}}"}}
  ]
}
```

HTML 使用 CSS 选择器，JSON 使用 JSON Pointer；字段类型支持 string、number、integer、boolean，以及保留嵌套值的 object、array、json。提取本身不保存结果，`extract.fields[].download: true` 只归一化 URL，不触发下载；保存和下载分别使用 emit 和 download 步骤。下一页地址也只提取为 next_url，需用后续步骤显式继续；工作流不自动按 URL 去重。

步骤可通过保留类型的 JSON Pointer 引用传递对象、数组和数值，使用 input/input_schema 做输入映射与校验，使用 transform 管道整理数据。网页支持从 JSON 树点选绑定、连续单步调试和仅生成请求；临时调试会话保留上下文与登录 Cookie，展示结果脱敏。可导入 [嵌套 JSON 查询 → 映射/校验 → POST → 保存示例](src/rule/examples/json-dataflow.json)，替换 example.com 地址后使用。响应保留与调试限额见 [JSON 数据流说明](docs/workflows.md#输入映射与类型约束)。

[工作流说明](docs/workflows.md) 包含列表/详情、请求参数、字面 vars、分支汇合、迭代计数、分页、认证头范围、资源限额、恢复与调试。HTTP/HTTPS 以外的协议不接受，HTTPS 默认校验证书。

## 现有数据升级

当前 SQLite `PRAGMA user_version = 3`。首次升级时，现有 v1 当前规则定义会在事务内转换成普通工作流并追加规则版本；历史版本、运行快照和结果不改写。未完成的旧运行标记为 failed，不重新执行。新建、更新、试采集 API 与网页导入拒绝旧版规则；迁移遇到无效旧定义时事务回滚，服务启动报错。

升级前停止服务并备份整个数据目录。升级后的数据库不承诺可由旧程序打开或回退；转换后的规则同样受单次 foreach 最多1000条、max_steps 最多10000个实例等限额约束。详细迁移边界见 [存储说明](src/storage/migrations/README.md)。

## 模块

所有源码、头文件、前端、测试和随项目维护的第三方源码都在 `src/`：

```text
src/
  main/          参数、启动、信号、单实例锁
  common/        JSON 辅助、时间、路径、URL、哈希
  model/         公共类型、状态
  service/       工作线程生命周期
  rule/          工作流校验、模板、旧定义升级、CSS 选择器校验
  scheduler/     定时调度入口
  engine/        步骤执行、试采集、分支汇合、重试、取消
  http/          有界 HTTP 请求和流式下载
  extract/       HTML/CSS、JSON Pointer、类型转换
  artifact/      临时文件、内容哈希、去重、登记
  storage/       SQLite、版本快照、队列、恢复、事务
  web/           API、访问控制、导出
    ui/          原生 HTML/CSS/JavaScript 管理端
  browser/       可选浏览器模块接口（首版未启用）
  tests/         C 单元测试和本地端到端测试
  third_party/   固定版本的 CivetWeb、Lexbor
```

`data/spider.db` 包含规则、历史版本、运行、持久化步骤实例、HTTP 尝试记录、运行会话、结果、文件元数据和事件；下载文件位于 `data/downloads/`。SQLite 使用 WAL、外键与短事务。运行固定使用启动时的规则快照，保存新规则不会改变已有运行。删除爬虫采用软删除，保留历史运行和文件。规则可能包含 Cookie/认证头，数据目录应仅允许运行用户读取；运行日志不输出完整规则。

结构化结果使用固定表＋JSON。步骤完成、上下文和输出、后续任务及 emit 记录在短事务中提交；网络请求不占用数据库事务。附件按运行 ID＋SHA-256 存储为 `.bin`，可从网页下载；同一运行内重复 URL 不重复下载。下载采用临时文件＋fsync＋重命名，启动时清理被中断的临时文件。当前没有自动保留期限或磁盘配额，应按需管理历史数据。备份时停止服务后复制整个数据目录，或使用 SQLite 在线备份方式；运行时只复制主 `.db` 文件可能丢失 WAL 数据。

## API

所有 API 支持 `Authorization: Bearer TOKEN`，配置 token 后全部 API 要求认证；写请求还需要 `X-Spider-Request: 1`，JSON 请求体最大 1 MiB。

| 方法 | 路径 | 用途 |
|---|---|---|
| GET | `/api/health` | 版本、浏览器能力 |
| GET / POST | `/api/spiders` | 列表、新建 |
| GET / PUT / DELETE | `/api/spiders/{id}` | 查看、更新、软删除 |
| POST | `/api/preview` | 提交 `{rule,step_id,context}`，试采集选中 request 步骤 |
| POST | `/api/preview/step` | 临时会话内单步调试，支持继续、分支选择及 dry_run |
| POST | `/api/preview/release` | 释放临时调试会话，提交 `{session_id}` |
| POST | `/api/spiders/{id}/run` | 手动启动 |
| GET | `/api/runs?spider_id={id}` | 最近 200 次运行 |
| GET | `/api/runs/{id}` | 状态、计数、规则快照 |
| POST | `/api/runs/{id}/stop` | 取消 |
| GET | `/api/runs/{id}/items?after={item_id}` | 按 ID 分页，每页最多 1000 条 |
| GET | `/api/runs/{id}/events` | 最近 200 条事件 |
| GET | `/api/runs/{id}/steps` | 工作流步骤实例元数据，最近1000条 |
| GET | `/api/runs/{id}/steps/{instance}` | 单个步骤输入、输出和错误 |
| GET | `/api/runs/{id}/artifacts` | 下载文件列表 |
| GET | `/api/artifacts/{id}/file` | 获取登记的文件 |
| GET | `/api/runs/{id}/export?format=json` | JSON 数组导出 |
| GET | `/api/runs/{id}/export?format=csv` | CSV 导出 |

单次导出上限 32 MiB，超过时返回错误而非静默截断；更大的数据通过结果分页 API 获取。CSV 使用运行快照的可选根 fields 选择并排列列名；未提供时从 emit 结果推导所有字段并排序。导出处理引号、换行和公式前缀。

## 验证

```sh
ctest --test-dir build --output-on-failure
cmake -S . -B build-asan -DSPIDER_SANITIZE=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-asan -j
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-asan --output-on-failure
```

C 核心和存储测试不需要 Python；端到端测试使用 Python 标准库启动本地 HTTP fixture，验证真实进程和 SQLite，不访问外部网站。Python 仅是测试工具，不是服务运行依赖。

## 当前范围

已实现统一工作流编辑、HTML/CSS 与 JSON Pointer 提取、单步骤试采集、列表进入详情、多接口汇合、受控分页与循环、独立下载、显式保存、定时计划、Cookie 会话、步骤级持久化队列、部分失败、重试、取消、按步骤恢复、版本快照、结果/日志/trace 查看和 JSON/CSV 导出。所有采集使用同一工作流执行器。

浏览器点选/录制、JavaScript 渲染、正则转换、cron/独立时区、分布式、多用户权限、按规则自定义文件名及完整 JSONPath 留待后续版本；配置 `backend` 为非 `http` 会明确报错。
