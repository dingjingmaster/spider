# 多步骤采集

网页使用统一工作流编辑器；新建规则默认是 `request → foreach → emit`。request 可直接填写地址、方法、格式与选择器，其他属性可在步骤 JSON 或完整 JSON 编辑器中配置。可以载入界面内的列表/详情示例，或导入 [list-detail.json](../src/rule/examples/list-detail.json)。示例的地址/选择器需要替换为实际目标站点，不是对 example.com 的可用采集承诺。

公开规则统一为 `schema_version: 2`，API 新建、更新、试采集以及网页导入均拒绝 v1 平面规则。SQLite 当前 `user_version = 3`：首次升级在事务内把现有 v1 当前定义转换成普通工作流，追加版本；历史版本、运行 snapshots/items 和 IDs 保持原样。未完成的旧运行标记 failed，不重放。任何无效旧定义导致升级事务回滚和启动报错。升级前停止服务并备份整个数据目录；升级后不承诺旧程序回退支持。详见 [迁移说明](../src/storage/migrations/README.md)。

## 定义与执行

一份规则包含 name、entry、steps，以及可选 vars/defaults/max_steps/interval_seconds/enabled。`entry` 指向起始步骤 ID。步骤 ID 使用字母/下划线开头，后续允许字母、数字、下划线和短横线，最长64字节。一份规则最多256个定义。

每个运行绑定规则快照。定义会实例化成持久化任务；foreach 的每条记录拥有独立 input 和步骤输出，parallel 的分支同样不共享可变 JSON。完成步骤、保存输出、创建后续任务、提交最终记录在同一 SQLite 事务中发生。分组等待期间不占工作线程。默认一个工作线程，一次执行一个步骤；需要并发请求时使用 `--workers 4` 等配置，同一工作流的独立分支及不同运行共享该线程池。delay_ms 是请求步骤自身的等待，不是全局域名限速；即使配置 parallel，单线程仍依次执行可领取的步骤。

## 步骤

| type | 配置 | 行为 |
|---|---|---|
| request | request、可选 extract、next | 独立 HTTP 请求、响应解析 |
| foreach | items、body、next | 对数组每个元素启动 body，全部分支结束后运行 next |
| parallel | branches、next | branches 对象的键为分支名、值为起始步骤，自动等待汇合后运行 next |
| condition | test、then、else | 条件成立/不成立时选择对应步骤，未提供分支则结束 |
| download | url、可选 headers/headers_origin、next | 使用当前运行会话下载文件，输出 id/url/sha256/bytes |
| join | fields、next | 组装对象作为中间输出，不写最终结果 |
| emit | fields、next | 显式保存最终记录，按运行/记录分支/保存步骤幂等 |

任何没有后续跳转的普通步骤都会结束当前分支；foreach/parallel 是结构化分组，body/branches 的终点不要再跳回分组自身。foreach 的输出是按原列表顺序排列的终点输出数组；parallel 输出是 `分支名 → 终点输出` 对象。例如 price 分支最终停在一个 request，汇合后通过 `{{steps.extra.price.items.0.price}}` 读取该输出。如果分支以 emit 结束，读取的是 emit 的字段对象。

所有步骤还支持可选 `next_input`：在写入当前步骤输出后解析该表达式，将结果作为后续步骤的 input。它只改变当前分支，不修改 run 变量或兄弟分支。例如 join 组装下一页 URL 后设置 `"next_input":"{{steps.advance}}"`，再返回 page 步骤。

`next` 可以回到已有步骤，实现分页等受控循环；不同循环迭代使用相同定义 ID，当前分支里的 `steps.ID` 表示最近一次该步骤的输出。默认最多1000个步骤实例，可用 max_steps 调整到1..10000（包含分组和保存步骤）；超限明确失败。单次 foreach 最多1000个元素，parallel 最多64个分支。这些限额也适用于由 v1 转换的规则：转换保留分页上限等策略，不豁免 max_steps 或单页列表展开上限。每个步骤上下文＋输出最多4 MiB，模板构造前累计计量大小；单次分组子上下文合计最多16 MiB，汇合输出最多4 MiB。

## 变量

- `run`：规则 vars，运行内的只读初始变量。vars 是字面 JSON，不会对其内容递归展开模板；例如 vars 中的字符串 `{{literal}}` 仍是该字符串。需要模板计算时应在步骤表达式中引用它。
- `input`：当前记录；foreach 会替换为当前元素，其他步骤沿用。
- `steps.ID`：当前分支已完成步骤的输出；子分支继承创建时的父上下文。

完整引用 `"{{input.id}}"` 保留 JSON 类型，包括数值、布尔、数组、对象和 null。嵌入文本 `"商品-{{input.id}}"` 只接受非 null 的标量。未定义变量报错；condition 的 exists 允许检测缺失变量。数组下标使用点分隔，例如 `steps.list.items.0.id`。这不是 JavaScript，不执行代码；变量路径的对象键使用字母、数字、下划线、短横线，含点号的任意外部键应先通过提取字段重命名。

request 支持：

```json
{
  "id": "price",
  "type": "request",
  "request": {
    "method": "POST",
    "url": "{{run.site}}/api/price",
    "headers": {"X-Token": "{{steps.login.items.0.token}}"},
    "query": {"language": "{{run.language}}"},
    "json": {"id": "{{input.id}}"}
  },
  "extract": {"format": "json", "fields": [{"name": "price", "selector": "/price", "type": "number"}]},
  "next": "save"
}
```

query 对象按字段进行 URL 编码，json 按 JSON 编码并补充 Content-Type；json 和原始 body 不能同时使用。GET/POST 是当前支持的方法。请求中的相对 URL 根据当前分支最近一次响应的最终 URL 解析。响应输出包含 items、next_url、iteration、response.status/url/body/bytes/truncated；原始 body 最多保留64 KiB，提取仍针对完整的有界响应。没有 extract 时不提取字段，items 为空；登录或纯 HTTP 步骤可在网页关闭“启用字段提取”，保存时保留 extract 缺省。extract 继续使用 CSS 或 JSON Pointer 和显式字段类型，默认字段类型是 string。

`iteration` 从1开始，是同一 run / record_key / branch_key / request 步骤定义的实例序号，重新进入该 request 会增加；同一实例的重试不增加序号。它不是 HTTP 尝试次数，也不合计兄弟分支，可用于分页条件 limit 比较。

defaults 和单个步骤可设置 timeout_seconds/retries/delay_ms/max_bytes，单步覆盖默认值。提取不自动保存；`extract.fields[].download: true` 只根据最终响应 URL 归一化该字段的附件 URL，不自动下载文件。使用独立 download 和 emit 步骤安排下载及保存。

Cookie 使用 libcurl 共享会话引擎，并按运行隔离，遵守 Cookie 域名/路径规则。活动运行的 Cookie 保存于 SQLite，以便重启后继续；运行结束后释放内存会话并清理 Cookie，启动恢复也会清理终态会话。Cookie 不出现在步骤 trace；规则/响应里的 Token 仍属于用户可调试的上下文，数据目录与 API token 应妥善管理。request.request 与 download 步骤都可配置 headers 和 headers_origin。未设置 headers_origin 时，自定义头只发送到该请求起始 URL 的同源目标；显式设置时，仅发送到 headers_origin 指定的 origin（协议、主机、端口一致），重定向逐跳按该 origin 检查，目标不匹配时不携带这些头。headers_origin 只限制自定义请求头，不授权转发 POST body。request 中另可设置 body_origin，独立限制允许接收 POST body 的 origin；默认值为当前 request.url 的 origin，初次请求与需要继续携带 body 的重定向都受其约束。v1 转换器把 body_origin 固定到原 start_url，保持跨源 POST 分页被拒绝。301/302/303 转成 GET 时不再携带原 POST body。新步骤的 URL、认证头与来源范围必须显式配置，Cookie 继续按会话规则发送。

下载示例：

```json
{"id":"file","type":"download","url":"{{input.file}}","headers":{"Authorization":"{{run.auth}}"},"headers_origin":"https://example.com"}
```

## 条件、分页和汇合

test 使用 `{left,op,right}`。op 支持 exists、eq、ne、gt、gte、lt、lte、contains。存在且非 null/非空字符串满足 exists（数值0和布尔false仍存在）；数值大小比较要求两侧都是数值。

分页使用 condition 检查 `{{steps.page.next_url}}`，由 join 组装 `{url:下一页地址}`，通过 next_input 将它传给同一个 page 步骤，形成 `page → foreach → condition → join → page` 循环；可在继续翻页前用数值比较限制页数，例如 vars 中设字面数值 `"limit":5`，再用条件 `{"left":"{{steps.page.iteration}}","op":"lt","right":"{{run.limit}}"}`。同时检查 next_url 是否存在；两个条件可连接成连续 condition 步骤。max_steps 仍约束整个流程的实例总数，包含 foreach 和 emit。分页不会自动对 URL 去重，重复 next_url 需要条件或实例限额终止。完整模板见 [pagination.json](../src/rule/examples/pagination.json)。控制器不隐式遍历 next_url，避免与用户配置的分支重复发请求。

多个补充请求可配置为：

```json
{"id":"extra","type":"parallel","branches":{"price":"price","stock":"stock"},"next":"save"}
```

price/stock 分支分别结束，extra 实例恢复后汇合为 `steps.extra.price` 和 `steps.extra.stock`。然后 emit 选择需要的字段，避免不同分支覆盖同名字段。

## 失败与恢复

步骤的 on_error：fail（默认）失败整个运行；skip 结束当前记录/分支并让其他记录继续；continue 将 `{error:...}` 写入该步骤输出后继续 next。skip/continue 使最终运行状态为 partial。每一步记录独立尝试次数、错误、输入输出和父分组关系。

网络失败按 retries 重试；解析、变量、数据库事务和资源限制错误不自动网络重试。POST 默认不自动重试；只有步骤显式 `retry_post:true` 才允许重放。服务中断时，已开始但尚未提交步骤的 POST 结果可能不确定，重启后默认标记失败，避免重复副作用；即使远端已返回成功，但本地步骤尚未提交，也不会静默重放。显式授权重试时，调用者应确保远端操作幂等。

GET 等可恢复步骤在重启后重新领取，已提交的步骤不再运行。停止运行将取消排队、运行及等待中的实例；晚到的结果不会再提交记录。取消只中止本地等待，不能撤销已经到达远端的请求。外部 HTTP 的副作用无法与 SQLite 形成同一个事务；本地 emit 的幂等不等同于远端请求只执行一次。

## 调试与 API

网页提供统一的工作流编辑器，支持 request 结构表单、逐步骤配置、完整 JSON 编辑、v2 规则导入导出、示例、选择 request 步骤并提供测试上下文。试采集只执行选中的 request，不执行前置登录/后续分支；使用临时 Cookie 会话，不保存试采集结果。需要先前输出时在 context 中提供 run/input/steps。

`POST /api/preview`：`{"rule": {...}, "step_id":"detail", "context":{"run":{},"input":{},"steps":{}}}`。

`GET /api/runs/{id}/steps` 返回最近1000个实例的元数据，不批量返回上下文。`GET /api/runs/{id}/steps/{instance_id}` 按需返回单个实例的 context/output/error；网页点击对应行后加载。认证要求与其他 API 相同。

结果仍通过 items/export API 获取。工作流根 fields 可用非空对象数组指定 CSV 列与顺序（`[{"name":"title"}]`）；每项 name 必须是非空字符串，显式配置无效时导出报错。仅未提供 fields 时推导所有最终结果的字段并排序。选择字段不改变保存的原始 JSON。
