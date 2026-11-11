# C Spider 首版设计

用户已确认：C 后端、SQLite、网页配置采集规则；模块按目录划分，所有源码（含前端、测试和随项目维护的第三方源码）位于 `src/`。

首版交付单机可运行程序：规则 CRUD/版本、HTML CSS 与 JSON Pointer 提取、试采集、手动/间隔定时运行、分页、HTTP 文件下载、限速/重试/停止、运行日志、JSON/CSV 导出、重启恢复。网页使用原生 HTML/CSS/JavaScript，由 C 服务提供。不依赖 Node/Python 运行时。浏览器渲染首版明确返回不支持，保留模块接口。

组件：C11/POSIX、libcurl、SQLite、json-c、Lexbor、CivetWeb。前两项候选之外，选择系统 json-c 以复用当前开发环境已有的 C JSON 库；JSON 提取首版使用标准 JSON Pointer，不声称支持完整 JSONPath。计划首版使用秒间隔，不声称支持 cron/独立时区。

规则包含 name/url/method/headers/body/format/row_selector/fields/next_selector/max_pages/interval_seconds/delay_ms/retries/timeout_seconds/max_bytes；field 包含 name/selector/attribute/type/required/download。JSON Pointer 针对列表行相对求值。HTML 字段选择器针对 row_selector 匹配的行相对求值。下载链接相对响应最终 URL 解析。列表结果以 JSON 存入固定表，文件落盘。

规则发布递增版本；运行保存规则快照。请求队列持久化，领取/重试/完成由数据库记录。单个工作线程顺序处理多个运行，控制站点负载；Web 请求独立处理。停止使用数据库状态＋curl 回调协作取消。运行中修改规则不影响快照。重复启动一个爬虫返回冲突。重启时未完成请求重置排队，停止请求保持取消。

HTTP 仅允许 http/https，TLS 校验开启，大小/超时/重定向受限。默认监听 127.0.0.1；远程绑定必须指定 API token。写 API 使用应用自定义请求头阻断跨站表单提交，非本地使用 token；对外返回的文件只能从登记的 artifact ID 获取。凭据不写运行日志。

实现细化：默认一个执行线程，`--workers` 允许用户显式增加到 16；每次运行内部顺序分页。自定义请求头按起始 URL 的协议/主机/端口限制同源发送，包括显式分页、附件和 HTTP 重定向；不转发跨源 POST 请求体。请求超时覆盖整个重定向链。下一页入队与本页结果提交使用同一 SQLite 事务。

存储：spiders/rule_versions/runs/requests/items/artifacts/run_events。SQLite WAL、外键、busy timeout；短事务；每个数据库操作受存储层互斥锁保护，线程不自行拼接 SQL。文件先写临时文件，成功后原子重命名，登记失败清理文件。运行完成必须包含写入结果成功，失败与取消有独立状态。

验收：本地 HTTP fixture 覆盖 HTML 列表、JSON、分页、文件、失败重试、大小限制、无效规则、更新快照、取消、重启恢复和导出；默认与 ASan/UBSan 构建通过。无需外部站点或真实数据库。
