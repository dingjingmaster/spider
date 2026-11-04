# Persistent workflows implementation plan

> Execute inline using superpowers:executing-plans; independent storage/UI tasks may use superpowers:dispatching-parallel-agents with the frozen interface contract.

**Goal:** Multi-step persistent C crawling while preserving v1 behavior.
**Spec:** docs/superpowers/specs/2026-11-04-workflows.md
**Architecture:** Step-level SQLite queue, owned JSON branch contexts, request/extraction adapters, per-run cookie sessions, automatic structured fork/join.

## Global constraints
- All source below src; no deployment/push/commit requested.
- Preserve existing v1 rules/data and cross-origin header policy.
- Bounded fanout, total instances and contexts; no network while holding SQLite mutex.

## Review focus
Branch isolation and group accounting; recovery without replay of committed steps; secrets and cookies across concurrent sessions; failed persistence must not report success; malformed/dangling/cyclic workflow definitions and bounded execution.

## Tasks
- [x] Rule validation/templates: src/rule/workflow.*, unit tests fail before implementation then pass.
- [x] Durable storage: src/storage/workflow.*, schema migration, storage tests; frozen interfaces in spec.
- [x] Execution/session: src/engine/workflow.*, src/http/session.*, service step dispatch; integration test fixture RED then GREEN.
- [x] Workflow editor and trace API: src/web/*; legacy editor still works, example workflow and trace views.
- [x] Independent review, complete debug/release/sanitizer tests and README examples.

Execution authorized by user's “实现”; continue without another approval gate.

## Execution evidence

- Baseline v1 suite: 3/3 before implementation. Rule tests failed before interfaces existed, then passed typed templates, references, conditions and null preservation.
- Storage tests cover actual v1 migration preserving IDs/results, independent and concurrent claims, nested/empty groups, stale leases, emit idempotency, retry/skip/continue, cancellation/restart, DB fault rollback, 4 MiB bounds and aggregate group bounds.
- Live workflow tests initially rejected/mis-executed new rules, then passed login Cookie/Token, encoded query/JSON POST, list/detail and relative links, parallel joins, file downloads, explicit emits, mixed success, bounded loops, per-step retry, graceful restart without replaying login, optional POST recovery authorization and stopping.
- Added next_input to pass an explicitly computed input into the next step. Four-page live fixture proves a single page definition can be revisited, while row scopes remain isolated.
- Independent final review identified unsafe POST replay, aggregate template allocation, artifact rollback race, JSON null ambiguity and ignored explicit CSV fields. Each was reproduced before repair. Fixes: persist/check prior POST attempts before resuming; charge render budget before cloning; serialize only artifact publication/register/rollback (HTTP still concurrent); separate missing/null lookup; honor explicit CSV fields. Deterministic real HTTP/files/SQLite artifact test forces failure after a peer registration.
- Trace list now metadata-only; one selected context/output is fetched on demand with run ownership checks. Node DOM tests check preservation, mode switches, import/export, preview context, hostile text and stale response rejection; no actual browser-rendering claim.
- Sessions are run-scoped and persisted only while active. Terminal cleanup also works when a restarted POST is refused before loading a session into memory; startup cleanup preserves active runs.
- Final Release CTest: 8/8, 37.67s. Final ASan/UBSan + leak detection CTest: 8/8, 38.21s. No first-party compile warnings; JS syntax and git diff whitespace checks pass.
- No deployment, commits or changes to vendored dependencies. Human-readable schema/API/examples: docs/workflows.md and src/rule/examples/.
