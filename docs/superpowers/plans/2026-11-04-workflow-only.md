# Workflow-only implementation plan

User authorized removing the overlapping simple version when workflows can replace it. All previous uncommitted workflow work is retained.

## Design

One public schema (workflow v2), one workflow editor and one instance executor. Keep HTTP/extraction validation as internal reusable primitives, not a second rule/execution mode. Remove legacy engine and queue APIs after moving regression coverage to the workflow path.

SQLite v3 performs a transactional one-time conversion of current v1 definitions into v2. Append rule versions, preserve historical snapshots/results/IDs. Unfinished v1 runs are marked failed with an explicit migration message; do not replay them or rewrite their snapshots. Invalid definitions abort the conversion transaction. Completed historical v1 data remains queryable.

Conversion uses ordinary request/foreach/condition/download/emit/join steps. Generated fields use safe internal names and emit original names. Preserve request method/headers/body, parsing/typed fields, file downloading, pagination cap, delays/retries and interval/enabled. Root vars hold literal legacy request values. `request` output adds `iteration` (same run/record/branch/definition), allowing ordinary conditions to limit pagination. Downloads support explicit headers plus headers_origin so legacy origin scoping is preserved. Existing workflow resource limits remain explicit.

## Tasks
- [x] Add failing conversion/public-schema/migration tests and adapt HTTP regression fixtures to workflow-only API.
- [x] Implement internal validators, one-time converter and iteration/header behavior; remove old execution path.
- [x] SQLite v3 migrate definitions; preserve historical data; remove unused legacy queue entrypoints.
- [x] Remove UI mode switch/simple editor; keep request-step forms, full JSON, previews and traces; update UI tests.
- [x] Update docs, independent review, Release and ASan/UBSan full test runs.

Use writing-plans/test-driven-development/executing-plans; UI is an independent task under dispatching-parallel-agents with frozen v2 API. No deployment or commits requested.

## Execution evidence and review

- Public-schema and literal-vars tests failed on the old behavior, then passed with separate internal HTTP/extraction validation. Conversion test failed on a stub and passed with ordinary v2 graph generation.
- The adapted real-service regression initially failed on missing `steps.page.iteration`, then passed after persisted-instance counting. Authentication-required pages/files validate explicit download headers both for new workflows and converted definitions.
- Storage tests cover v1/v2 upgrades, atomic rollback on invalid definitions/JSON, byte-identical historical snapshots/versions, unfinished legacy runs failed without replay, and repeat-open idempotency.
- Independent final review found POST cross-origin pagination body forwarding and UI injection of empty extract into requests that do not parse responses. Both receive regression tests and fixes before completion.
- Ruling: add independent `request.body_origin`, defaulting to the current request URL, and set it to the original start URL in the converter. Custom-header scoping does not authorize sending POST bodies to a different origin. The POST migration regression reproduced the leak before this fix.
- Ruling: archival CSV handling of historical flat snapshots remains read-only compatibility; no legacy rule editor or executor remains.
- Final validation: default build 9/9 (23.39s), Release 9/9 (23.45s), ASan/UBSan with leak detection 9/9 (23.71s); final UI revision also passes Node syntax and round-trip regressions. Builds reported no compiler warnings. No real-browser visual check was performed.
- Independent review rechecked both fixes and reported no blocking findings. All changes remain uncommitted; no installed service or user database was opened or changed during tests.
