# Spider

使用 C 实现的单机采集工作台：在网页配置请求与解析规则，试采集，定时或手动执行，将结构化数据保存到 SQLite，将附件保存到本地磁盘。

## 构建和启动

需要 Linux/POSIX 环境、C11 编译器、CMake >= 3.16、pkg-config，以及开发库：libcurl >= 7.85、SQLite >= 3.35、json-c >= 0.15、OpenSSL >= 1.1.1。CivetWeb 1.16 和 Lexbor 3.0.0 的源码已随项目提供，构建时不下载依赖。

Debian/Ubuntu 可安装：

```sh
sudo apt install build-essential cmake pkg-config libcurl4-openssl-dev libsqlite3-dev libjson-c-dev libssl-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/spider --data-dir ./data --port 8080
```

打开 **http://127.0.0.1:8080**。点击左侧 `＋` 新建爬虫，填写请求地址和字段，使用“试采集”查看结果；保存后可立即运行，或者设置执行间隔。

```sh
./build/spider --help
./build/spider --host 0.0.0.0 --port 8080 --data-dir ./data --token YOUR_TOKEN
```

远程绑定必须提供 token，在网页“连接设置”中填写同一口令。非本机访问应通过 HTTPS 反向代理；程序内置监听器提供 HTTP。token 通过 Authorization 请求头传递，不放 URL。浏览器将口令保存在当前会话的 sessionStorage 中。程序继承 libcurl 的代理环境设置；本地测试使用 `NO_PROXY=127.0.0.1,localhost`。

默认一个工作线程，可通过 `--workers 1..16` 显式增加并发。每个运行内部按页顺序执行，`delay_ms` 控制该运行内的页面请求间隔，不是跨爬虫的全局域名限速。一个数据目录只允许一个进程使用。Ctrl+C/SIGTERM 停止服务时保留未完成任务，下次启动恢复；网页“停止运行”会将该运行取消。

## 规则示例

HTML：字段选择器相对于每个 `.product` 行。空选择器指向当前行；属性留空提取文本；`download: true` 将字段解释为附件 URL，正式运行时下载。

```json
{
  "name": "商品列表",
  "url": "https://example.com/products",
  "method": "GET",
  "format": "html",
  "row_selector": ".product",
  "fields": [
    {"name": "title", "selector": "h2", "required": true},
    {"name": "price", "selector": ".price", "type": "number"},
    {"name": "file", "selector": "a.download", "attribute": "href", "download": true}
  ],
  "next_selector": "a.next",
  "max_pages": 5,
  "interval_seconds": 0,
  "enabled": true,
  "delay_ms": 300,
  "retries": 2,
  "timeout_seconds": 30,
  "max_bytes": 4194304,
  "headers": {},
  "body": ""
}
```

JSON：使用 **JSON Pointer**，不是 JSONPath。`row_selector` 指向数组时逐条提取；字段路径相对于当前条目。例如响应 `{"items":[{"title":"A","price":12.5}]}`：

```json
{
  "name": "接口采集",
  "url": "https://example.com/api/items",
  "format": "json",
  "row_selector": "/items",
  "fields": [
    {"name": "title", "selector": "/title", "required": true},
    {"name": "price", "selector": "/price", "type": "number"}
  ]
}
```

类型支持 `string`（默认）、`number`、`integer`、`boolean`。文本首尾空白会移除；必填字段缺失、类型转换失败都会令当前页失败。JSON Pointer 中 `/` 写作 `~1`，`~` 写作 `~0`。JSON 的 `next_selector` 是指向下一页 URL 字符串的根级 JSON Pointer；HTML 的下一页选择器读取第一个匹配元素的 `href`。相对 URL 根据响应的最终 URL 解析。分页 URL 去重，并受 `max_pages` 限制。分页继续使用规则的请求方式、请求头和请求体。

试采集只请求一个页面，不跟随分页、不下载附件、不保存结果；原始响应预览最多 64 KiB。HTTP 失败最多额外重试 `retries` 次；解析、文件持久化、SQLite 错误会令运行失败。页面响应与单个文件下载都受 `max_bytes` 和超时限制。HTTP/HTTPS 以外的协议不接受，HTTPS 默认校验证书。自定义请求头仅发送给规则起始 URL 的同源地址（协议、主机、端口均一致）；跨源分页、附件及重定向不携带这些请求头，跨源 POST 请求体转发被拒绝。

## 模块

所有源码、头文件、前端、测试和随项目维护的第三方源码都在 `src/`：

```text
src/
  main/          参数、启动、信号、单实例锁
  common/        JSON 辅助、时间、路径、URL、哈希
  model/         公共类型、状态
  service/       工作线程生命周期
  rule/          规则与 CSS 选择器校验
  scheduler/     定时调度入口
  engine/        试采集、分页执行、重试、取消
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

`data/spider.db` 包含规则、历史版本、运行、持久化请求队列、结果、文件元数据和事件；下载文件位于 `data/downloads/`。SQLite 使用 WAL、外键与短事务。运行固定使用启动时的规则快照，保存新规则不会改变已有运行。删除爬虫采用软删除，保留历史运行和文件。规则可能包含 Cookie/认证头，数据目录应仅允许运行用户读取；运行日志不输出完整规则。

结构化结果使用固定表＋JSON。每个页面的结果、完成状态和下一页入队在同一事务提交。附件按运行 ID＋SHA-256 存储为 `.bin`，可从网页下载；同一运行内重复 URL 不重复下载。下载采用临时文件＋fsync＋重命名，启动时清理被中断的临时文件。当前没有自动保留期限或磁盘配额，应按需管理历史数据。备份时停止服务后复制整个数据目录，或使用 SQLite 在线备份方式；运行时只复制主 `.db` 文件可能丢失 WAL 数据。

## API

所有 API 支持 `Authorization: Bearer TOKEN`，配置 token 后全部 API 要求认证；写请求还需要 `X-Spider-Request: 1`，JSON 请求体最大 1 MiB。

| 方法 | 路径 | 用途 |
|---|---|---|
| GET | `/api/health` | 版本、浏览器能力 |
| GET / POST | `/api/spiders` | 列表、新建 |
| GET / PUT / DELETE | `/api/spiders/{id}` | 查看、更新、软删除 |
| POST | `/api/preview` | 提交规则试采集 |
| POST | `/api/spiders/{id}/run` | 手动启动 |
| GET | `/api/runs?spider_id={id}` | 最近 200 次运行 |
| GET | `/api/runs/{id}` | 状态、计数、规则快照 |
| POST | `/api/runs/{id}/stop` | 取消 |
| GET | `/api/runs/{id}/items?after={item_id}` | 按 ID 分页，每页最多 1000 条 |
| GET | `/api/runs/{id}/events` | 最近 200 条事件 |
| GET | `/api/runs/{id}/artifacts` | 下载文件列表 |
| GET | `/api/artifacts/{id}/file` | 获取登记的文件 |
| GET | `/api/runs/{id}/export?format=json` | JSON 数组导出 |
| GET | `/api/runs/{id}/export?format=csv` | CSV 导出 |

单次导出上限 32 MiB，超过时返回错误而非静默截断；更大的数据通过结果分页 API 获取。CSV 字段引用自该运行的规则快照，并处理引号、换行和公式前缀。

## 验证

```sh
ctest --test-dir build --output-on-failure
cmake -S . -B build-asan -DSPIDER_SANITIZE=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-asan -j
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-asan --output-on-failure
```

C 核心和存储测试不需要 Python；端到端测试使用 Python 标准库启动本地 HTTP fixture，验证真实进程和 SQLite，不访问外部网站。Python 仅是测试工具，不是服务运行依赖。

## 首版范围

已实现请求/规则编辑、HTML/CSS 与 JSON Pointer 提取、单页试采集、URL 分页、文件下载、手动及秒间隔计划、多运行并发、HTTP 重试、取消、重启恢复、版本快照、结果/日志查看、JSON/CSV 导出和规则导入导出。

浏览器点选/录制、JavaScript 渲染、跨页 Cookie 会话、列表进入详情的多步骤工作流、正则转换、cron/独立时区、分布式、多用户权限、按规则自定义文件名及完整 JSONPath 留待后续版本；配置 `backend` 为非 `http` 会明确报错。
