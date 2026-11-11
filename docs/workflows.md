# 工作流引擎协议

本页描述 `spiders/workflow/` 通用引擎，可由 C 代码登记固定工作流任务。产品 Web 只显示状态，不提供新建、编辑、导入、绑定或调试功能。代码调用方式见 [模块说明](modules.md)。

下文中的预览与历史 HTTP 示例是 tests/support 引擎测试夹具的协议，只有测试程序提供这些路由，不属于产品 Web API。工作流运行数据和规则格式继续用于恢复及引擎回归验证。

JSON 工作流使用 `schema_version: 2`。SQLite 核心 `user_version=3`，旧定义升级和历史快照保持原有边界；启动代码目录同步之后，旧网页任务不参与调度。

## 定义与执行

一份规则包含 name、entry、steps，以及可选 vars/defaults/max_steps/interval_seconds/enabled。`entry` 指向起始步骤 ID。步骤 ID 使用字母/下划线开头，后续允许字母、数字、下划线和短横线，最长64字节。一份规则最多256个定义。

每个运行绑定规则快照。定义会实例化成持久化任务；foreach 的每条记录拥有独立 input 和步骤输出，parallel 的分支同样不共享可变 JSON。完成步骤、保存输出、创建后续任务、提交最终记录在同一 SQLite 事务中发生。

默认 4 个工作线程，一次完整爬虫运行占用一个工作线程。同一运行内部所有步骤、foreach/parallel 分支、网络请求和重试等待均在该线程串行推进；不同爬虫配置可以在不同线程并发。parallel 保留分支汇合语义，不执行并发请求。delay_ms 是请求步骤自身的等待，不是全局域名限速。调度器合并已有任务期间的定时触发，取消后保留爬虫占用直到回调真正退出。详见 [模块说明](modules.md)。

## 步骤

| type | 配置 | 行为 |
|---|---|---|
| request | request、可选 extract、next | 独立 HTTP 请求、响应解析 |
| foreach | items、body、next | 为数组每个元素建立 body，串行推进，全部分支结束后运行 next |
| parallel | branches、next | branches 对象的键为分支名、值为起始步骤，串行推进各分支，汇合后运行 next |
| condition | test、then、else | 条件成立/不成立时选择对应步骤，未提供分支则结束 |
| download | url、可选 headers/headers_origin、next | 使用当前运行会话下载文件，输出 id/url/sha256/bytes |
| transform | 可选 value、operations、next | 对 JSON 值执行有界转换管道，不写最终结果 |
| join | fields、next | 组装对象作为中间输出，不写最终结果 |
| emit | fields、next | 显式保存最终记录，按运行/记录分支/保存步骤幂等 |

任何没有后续跳转的普通步骤都会结束当前分支；foreach/parallel 是结构化分组，body/branches 的终点不要再跳回分组自身。foreach 的输出是按原列表顺序排列的终点输出数组；parallel 输出是 `分支名 → 终点输出` 对象。例如 price 分支最终停在一个 request，汇合后通过 `{{steps.extra.price.items.0.price}}` 读取该输出。如果分支以 emit 结束，读取的是 emit 的字段对象。

所有步骤支持可选 `input`、`input_schema`，执行前准备并校验当前步骤输入，详见下文。还支持可选 `next_input`：在写入当前步骤输出后解析该表达式，将结果作为后续步骤的 input。它只改变当前分支，不修改 run 变量或兄弟分支。例如 join 组装下一页 URL 后设置 `"next_input":"{{steps.advance}}"`，再返回 page 步骤。

`next` 可以回到已有步骤，实现分页等受控循环；不同循环迭代使用相同定义 ID，当前分支里的 `steps.ID` 表示最近一次该步骤的输出。默认最多1000个步骤实例，可用 max_steps 调整到1..10000（包含分组和保存步骤）；超限明确失败。单次 foreach 最多1000个元素，parallel 最多64个分支。这些限额也适用于由 v1 转换的规则：转换保留分页上限等策略，不豁免 max_steps 或单页列表展开上限。每个步骤上下文＋输出最多4 MiB，模板构造前累计计量大小；单次分组子上下文合计最多16 MiB，汇合输出最多4 MiB。

## 变量

- `run`：规则 vars，运行内的只读初始变量。vars 是字面 JSON，不会对其内容递归展开模板；例如 vars 中的字符串 `{{literal}}` 仍是该字符串。需要模板计算时应在步骤表达式中引用它。
- `input`：当前记录；foreach 会替换为当前元素，其他步骤沿用。
- `steps.ID`：当前分支已完成步骤的输出；子分支继承创建时的父上下文。

完整引用 `"{{input.id}}"` 保留 JSON 类型，包括数值、布尔、数组、对象和 null。嵌入文本 `"商品-{{input.id}}"` 只接受非 null 的标量。未定义变量报错；condition 的 exists 允许检测缺失变量。数组下标使用点分隔，例如 `steps.list.items.0.id`。这不是 JavaScript，不执行代码；变量路径的对象键使用字母、数字、下划线、短横线，点模板不支持含点号的对象键；这类键或包含斜杠、波浪号的键可用 JSON Pointer 引用。

### 保留类型的 JSON Pointer 引用

引用对象使用 `{"$ref":"/input/a.b~1~0","default":0}`，对应 input 中名为 `a.b/~` 的键。路径使用 JSON Pointer：`/` 编码为 `~1`，`~` 编码为 `~0`；点号是键本身的字符，数组下标使用 `/0`。引用根限定为 `/run`、`/input`、`/steps` 及其子路径，不引用整个上下文或内部状态字段。引用对象只能包含 `$ref` 和可选 `default`，其中 default 是字面 JSON 值，不展开模板。

引用保留原值的 JSON 类型；对象、数组、数值、布尔和 null 不会被转成文本。仅路径**不存在**时使用 default；已有 null、false、0 或空字符串都不会被 default 替换。未提供 default 且路径不存在时报错。URL 和原始 body 也可使用引用对象，但解析后的值必须是该请求参数接受的类型，例如 URL 和 body 需为字符串。query、headers 和 json 的字段可使用相同引用；query 将标量编码为 URL 参数并省略 null，headers 要求最终值是合法字符串，json 保留嵌套结构。数值等字段要用于请求头时需显式转换为字符串。

## 输入映射与类型约束

步骤根级 `input` 是表达式或 JSON 投影，先对进入该步骤的上下文求值，再替换当前分支 input；省略时沿用原 input。`input_schema` 可以单独出现，只校验已有输入。校验先于 HTTP 等步骤工作，失败时不发送请求。子分支仍拥有独立上下文；映射不修改 run 或兄弟分支。

```json
{
  "id": "prepare", "type": "join",
  "input": {"id": {"$ref": "/steps/lookup/items/0/id"}, "enabled": false},
  "input_schema": {
    "type": "object", "required": ["id", "enabled", "count"], "additionalProperties": false,
    "properties": {"id": {"type": "integer"}, "enabled": {"type": "boolean"}, "count": {"type": "integer", "default": 1}}
  },
  "fields": {"id": {"$ref": "/input/id"}, "count": {"$ref": "/input/count"}}
}
```

支持的约束是一个明确的小集合：

| 关键字 | 支持内容 |
|---|---|
| type | string、number、integer、boolean、object、array、null；或由这些名称组成的非空数组 |
| properties | 对象字段名到子约束的映射 |
| required | 必须存在的对象字段名数组 |
| items | 数组中所有元素共用的子约束 |
| default | 字面默认值，仅补缺失属性，不替换 null、false 或0 |
| additionalProperties | 布尔值；false 拒绝 properties 未声明的对象字段 |

不支持的关键字会报错；不执行隐式类型转换，例如字符串 `"12"` 不满足 integer。需要转换时使用显式 transform/cast。约束默认值也需满足自身类型要求。未设置 input/schema/next_input 表示省略；显式设置 `null`、`false`、`0` 或 `{}` 则保留该值。

步骤完成时先保存到 `steps.ID`，再计算 `next_input`，最后按可选根级 `retain_steps` 裁剪上下文中的步骤输出。例如 `"next_input":{"$ref":"/steps/prepare"},"retain_steps":["prepare"]` 将准备结果交给下一步，同时释放其他步骤的上下文副本。省略 retain_steps 保留现有输出，空数组清空 steps；列表只能包含本规则已定义的步骤 ID。裁剪不删除持久化 trace；已识别的敏感值标记也会跨裁剪保留。不要把某个值只保留在已裁剪的 steps 中，随后又引用它。

## JSON 转换管道

transform 的根级可选 value 指定初始表达式，省略时使用当前 input。operations 按顺序处理前一项的结果，最后结果保存为 `steps.ID`；需要继续用它作为 input 时设置 next_input。

| op | 参数示例 | 行为 |
|---|---|---|
| pick | `{"op":"pick","path":"/customer/id","default":0}` | 对当前值按 JSON Pointer 取值；仅缺失路径时使用可选字面 default |
| default | `{"op":"default","value":[]}` | 当前值为 null 时替换为字面 value；不替换 false、0 或空字符串 |
| rename | `{"op":"rename","from":"old.key","to":"new/key"}` | 改对象的直接键名，不按路径解释；源缺失或目标已存在时报错，同名重命名保持原样 |
| cast | `{"op":"cast","type":"integer"}` | 显式转换标量为 string、number、integer、boolean 或 null；拒绝无效转换 |
| trim | `{"op":"trim"}` | 移除字符串首尾空白 |
| replace | `{"op":"replace","old":"old","new":"new"}` | 全部替换字面字符串；old 必须非空，不使用正则 |
| filter | `{"op":"filter","test":{"left":{"$ref":"/input/enabled"},"op":"eq","right":true}}` | 逐项筛选数组，test 中 input 是当前元素 |
| map | `{"op":"map","fields":{"id":{"$ref":"/input/id"}}}` | 逐项映射数组，fields 中 input 是当前元素 |

cast 不接受对象/数组；number/integer 的字符串输入必须是完整有效数字，不接受前后空白、非有限值或越界，integer 还拒绝小数。boolean 仅接受布尔值或字符串 `"true"` / `"false"`；null 只接受原 null，其他类型的 null 转换需要先显式 default。schema 校验不会代替 cast。每条管道1..32项；转换数组最多1000元素，值/上下文最多4 MiB，嵌套最多32层。映射、默认值展开和输出构造均受限，失败走步骤的 on_error 策略。

## 请求、提取与响应保留

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

query 对象按字段进行 URL 编码，json 按 JSON 编码并补充 Content-Type；json 和原始 body 不能同时使用。GET/POST 是当前支持的方法。请求中的相对 URL 根据当前分支最近一次响应的最终 URL 解析。响应输出包含 items、next_url、iteration、response.status/url/bytes/truncated；默认还保留最多64 KiB 的 response.body，提取仍针对完整的有界响应。没有 extract 时不提取字段，items 为空；登录或纯 HTTP 步骤可以省略 extract。extract 使用 CSS 或 JSON Pointer，字段默认类型是 string。除 string/number/integer/boolean 外，type 支持 object、array、json：JSON 响应保留原生嵌套值，HTML 文本则需能解析为 JSON。object/array 要求形状匹配，json 可保留任意 JSON 值。非必填的缺失/null 输出为 null，required 字段缺失/null 报错。

请求步骤**根级** `response_json:true` 将完整且受 max_bytes 限制的响应解析并保留为 `response.json`；它不受64 KiB 正文预览上限截断，响应不是合法 JSON 时失败。根级 `response_body:false` 省略 response.body，默认仍保留原正文预览。这两个布尔选项与 request/extract 同级，不放在 request 对象中；可以关闭字段提取，仅保留完整响应 JSON。

完整 [json-dataflow.json](../src/rule/examples/json-dataflow.json) 展示 `lookup → prepare → submit → save`：查询提取 object/array/json，映射并校验输入，transform 重命名，POST json 保留嵌套类型，再 emit 保存。先把 vars.site 替换为实际服务。查询端点 `/api/lookup` 应返回以下形状；/api/submit 接受 JSON POST 并返回合法 JSON：

```json
{
  "payload": {
    "account": {"id": 7, "profile": {"display.name": "Ada", "a/b~": true}},
    "lines": [{"sku": "A"}]
  },
  "labels": ["sample"],
  "enabled": false
}
```

示例中缺失的 count 由 schema 补1，缺失的 revision 由引用补0，原 false 不会被默认 true 覆盖。POST 的 customer、lines、labels 分别保持对象/数组，enabled 保持布尔，revision 保持整数；包含点号或 `/~` 的 profile 键使用 Pointer 引用。URL 为 example.com 占位，不代表公共站点提供这些接口。

`iteration` 从1开始，是同一 run / record_key / branch_key / request 步骤定义的实例序号，重新进入该 request 会增加；同一实例的重试不增加序号。它不是 HTTP 尝试次数，也不合计兄弟分支，可用于分页条件 limit 比较。

defaults 和单个步骤可设置 timeout_seconds/retries/delay_ms/max_bytes，单步覆盖默认值。提取不自动保存；`extract.fields[].download: true` 只根据最终响应 URL 归一化该字段的附件 URL，不自动下载文件。使用独立 download 和 emit 步骤安排下载及保存。

Cookie 使用 libcurl 共享会话引擎，并按运行隔离，遵守 Cookie 域名/路径规则。活动运行的 Cookie 保存于 SQLite，以便重启后继续；运行结束后释放内存会话并清理 Cookie，启动恢复也会清理终态会话。调试与 trace 返回脱敏展示，Cookie 仍在服务端会话中用于后续请求；这不会改写实际执行值或替代数据目录及 API token 的访问控制。request.request 与 download 步骤都可配置 headers 和 headers_origin。未设置 headers_origin 时，自定义头只发送到该请求起始 URL 的同源目标；显式设置时，仅发送到 headers_origin 指定的 origin（协议、主机、端口一致），重定向逐跳按该 origin 检查，目标不匹配时不携带这些头。headers_origin 只限制自定义请求头，不授权转发 POST body。request 中另可设置 body_origin，独立限制允许接收 POST body 的 origin；默认值为当前 request.url 的 origin，初次请求与需要继续携带 body 的重定向都受其约束。v1 转换器把 body_origin 固定到原 start_url，保持跨源 POST 分页被拒绝。301/302/303 转成 GET 时不再携带原 POST body。新步骤的 URL、认证头与来源范围必须显式配置，Cookie 继续按会话规则发送。

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

## 引擎调试与测试夹具协议（非产品 API）

测试夹具的单步预览只执行选中的 request：`POST /api/preview` 接受 `{rule,step_id,context}`，不自动执行前置或后续步骤。

连续调试使用 `POST /api/preview/step`。首次请求包含 rule、step_id，可选 context；后续请求携带相同 rule、选择的 step_id 和服务端返回的 session_id，**不再携带 context**。返回 `{session_id,step_id,output,context,request,next_steps,notice}`。返回的 context/output/request 是展示副本；不要把脱敏后的 Token 当作真实值传回，真实上下文与临时 Cookie 由服务端继续保留。

可选参数 dry_run 为 true 时仅解析 request，不发送 HTTP，也不推进会话；返回 request 可检查 URL、编码后的参数及请求体。foreach 仅预览 item_index 指定的一项，省略时为0，并返回单条预览提示；parallel 必须提供 branch。调试每次只执行一个 request/transform/join/emit/condition 或选定分支的分组入口，不会自动运行整个工作流，不下载文件，emit 不写最终结果；next_steps 只是可继续步骤的建议。

服务端最多16个临时调试会话，闲置10分钟失效，每会话最多256个已执行步骤；上下文/输出仍受4 MiB 等数据限额约束。调试会话不持久化；修改规则需创建新会话。历史调试器在规则或初始上下文修改、点击重置时异步释放已知会话，可重新开始；迟到且不再采用的预览结果也释放其会话，不会覆盖新调试结果或释放当前有效会话。释放失败不阻塞编辑，闲置会话仍由超时机制回收。

`POST /api/preview/release` 接受 `{"session_id":"返回的会话ID"}`，释放该临时会话，成功返回200；不需要 rule。与其他写 API 一样需要认证和 `X-Spider-Request: 1`。

测试夹具可以传入初始上下文 `{"input":{},"steps":{}}`，缺少 run 时使用规则 vars。缺少前置输出时需先执行前置步骤，或在初始 steps 提供测试数据。foreach 选择 item_index，parallel 选择 branch；这些能力通过测试夹具请求调用，没有对应产品页面。

仅测试夹具提供 `GET /api/runs/{id}/steps` 和单个步骤实例 trace；产品状态页只读取状态与事件，不提供上下文绑定和步骤编辑。

新连续预览和步骤 trace 使用递归脱敏副本，不改变执行或持久化结果。内置敏感字段名（例如 Authorization、Cookie、token/password/secret 等）会遮蔽对应值；规则根可用 `"sensitive_fields":["sessionId","signingValue"]` 增加字段名。JSON 字符串正文也会解析后脱敏，已识别的敏感值在复制到其他字段、拼入请求或跨 retain_steps 裁剪后仍保持遮蔽。任意非 JSON 文本中的未知秘密无法全部推断；不需要保留正文时配置步骤根级 response_body:false。脱敏不意味着规则、结果数据库或所有外部响应都已清除秘密。

sensitive_fields 最多64个名称，每个1..80字节。分支内敏感来源记录最多512个标量值、合计1 MiB，超限会明确报错。transform 消费敏感输入或在操作中引用敏感字段时，会保守遮蔽其派生输出；用于下一步请求的真实值和类型仍保持原样。

引擎结果可通过测试夹具的 items/export API 获取（产品 Web 不提供这些接口）。工作流根 fields 可用非空对象数组指定 CSV 列与顺序（`[{"name":"title"}]`）；每项 name 必须是非空字符串，显式配置无效时导出报错。仅未提供 fields 时推导所有最终结果的字段并排序。选择字段不改变保存的原始 JSON。
