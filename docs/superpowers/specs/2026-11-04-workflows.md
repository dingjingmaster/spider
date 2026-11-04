# Persistent workflow rules v2

Approved direction: C backend, all sources under src; rule-based request/foreach/condition/parallel/download/emit with scoped variables and persistent step instances. Preserve existing v1 rules and tests. User explicitly requested implementation.

## Rule contract
Root: `schema_version:2`, `name`, `entry`, `steps:[]`, optional `vars:{}`, `defaults:{timeout_seconds,retries,delay_ms,max_bytes}`, `max_steps` (default 1000, max 10000), interval_seconds/enabled (same as v1).

Steps:
- request: `request:{url,method,headers,body,query,json}`, `extract:{format,row_selector,fields,next_selector}`, optional `next` step ID. Each request resolves independently; extraction output is `{items:[],next_url,response:{status,url,body,truncated}}`. `query` is URL-encoded, `json` is JSON-encoded; cross-origin custom headers remain restricted to the step request's starting origin.
- foreach: `items` expression resolving to array, `body` step ID, optional `next`. Child branches receive `input` = each element and a stable record_key. Group waits for all branches before continuing.
- parallel: `branches:{name:entry_step,...}`, optional `next`. Forks independent contexts and joins automatically; group output maps branch names to terminal branch outputs.
- condition: `test:{left,op,right}`, op=exists/eq/ne/gt/gte/lt/lte/contains, `then`/`else` optional step IDs.
- download: `url` template; output artifact metadata, optional `next`.
- emit: `fields` object of expressions; idempotently saves one final record, optional `next`.
- join: `fields` object; explicit merge transform without persistence, optional `next`.
All steps: ID [A-Za-z_][A-Za-z0-9_-]{0,63}; `on_error`=fail(default)/skip/continue; retries optional, POST retries only with `retry_post:true`. Optional `next_input` resolves after storing current step output and replaces input only for this continuation; this implements cursor/URL transitions without global mutable variables. A step with no transition ends its branch. Loops use ordinary references bounded by max_steps.

Templates: exact `{{input.id}}` preserves JSON type; embedded templates interpolate scalar text only. Roots are `run` (root vars), `input` (current record), `steps` (outputs on this branch). Array indices use numeric path segments. Missing variables are errors except condition exists. No code execution. Output context is an owned JSON clone; siblings never share mutable JSON. Group output also appears as `steps.<groupID>` on the resumed parent context.

## Storage/execution contract for independent implementation
`src/storage/workflow.h` exposes:
```
int wf_initialize(Store*,char*,size_t); // queued v2 runs -> running, root job once; restart resumes
json_object *wf_claim(Store*,char*,size_t); // one ready instance, NULL(empty/error distinguished by error)
int wf_finish(Store*,json_object *job,json_object *output,json_object *context,
              json_object *children,json_object *emitted,bool page,char*,size_t);
int wf_fork(Store*,json_object *job,json_object *children,char*,size_t);
json_object *wf_group_results(Store*,int64_t group,char*,size_t);
int wf_failure(Store*,json_object *job,const char *policy,const char *message,
               bool retry,int backoff_ms,char*,size_t);
int wf_cancel(Store*,int64_t run,char*,size_t);
```
All pointers borrowed except returned JSON. Job fields: id/run_id/step_id/parent_id(nullable)/branch_key/record_key/context(JSON)/phase(0 normal,1 resume group)/attempts/snapshot(JSON). Children array entries: step_id/context/record_key/branch_key. wf_finish normal continuations inherit parent_id/branch_key; wf_fork children use current job.id as parent_id and supplied branch keys. wf_finish marks done, persists output/context, inserts continuations and emitted object, increments pages for successful request. Terminal child updates group's pending; zero pending sets group ready phase1. Group results: foreach array in branch index order, parallel object mapping keys to terminal outputs (engine converts array if necessary). A skipped branch's output is `{error:message}`; run eventually partial. Continue is handled by engine calling wf_finish(error output, normal transitions) and recording warning through wf_failure policy=continue. Failed runs cancel queued/waiting work; running jobs must observe terminal run status and cannot commit after cancellation/failure.

Database v2 migration must preserve v1 data. Add step_instances, sessions(run_id,cookies), and items.step_instance_id/record_key with nullable legacy request_id. Emitted records unique on (run_id,record_key,step_id) for idempotency. `requests` gets optional step_instance_id/method/request_key for workflow HTTP attempts (engine may log actual requests via own API); retain v1 UNIQUE(run_id,url) semantics only for legacy engine.

`wf_claim` and completion use short transactions. Recovery: running jobs -> queued; cancelled runs never reactivated. Group wait counts/pending are durable. Final run status succeeded/partial/failed/cancelled; complete only with no ready/running/waiting jobs. Max_steps limits created instances, including fanout. Cap group fanout to 1000. No network under store mutex. Claims across workers may execute independent instances in same run concurrently. Context input/output is size-bounded to 4 MiB per instance; traces omit session cookies and actual request authentication headers.

Service: each worker calls wf_initialize then wf_claim/execute once, otherwise legacy store_claim_run (exclude v2). No worker blocks while waiting on descendants. Sessions per run use libcurl shared-cookie engine with locking and SQLite persistence, disposed after workers stop. Preview executes one chosen request against supplied context (no output persistence/followups/download), API body `{rule,step_id,context}`; plain v1 preview remains supported.

Web: retain simple editor, add workflow mode with per-step structured configuration or complete JSON editor; include list/detail example and validation errors. `/api/runs/{id}/steps` -> latest 1000 instance metadata incl context/output/error for debugging, rows JSON decoded by store_query for keys context/output. Existing items/export endpoints support workflow items; CSV columns derive from root fields if supplied or emitted item keys. root.fields is optional for workflows and used only for exports.

## Verification
Tests use local HTTP fixtures and SQLite: list/detail/API merge, parallel fork/join, scoped sibling data, session+token requests, encoded query/JSON body, controlled pagination, explicit download, failure/skip/partial, step retry, immutable snapshot, graceful/crash recovery without replaying successful steps, migration old data, old-rule regression, malformed references/templates and limits. ASan/UBSan complete suite required.
