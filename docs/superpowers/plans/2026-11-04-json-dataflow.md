# JSON Dataflow Implementation Plan

> **For agentic workers:** use superpowers:executing-plans and test-driven-development; independent data helpers/UI tasks may use dispatching-parallel-agents. One final independent review.

**Goal:** connect extraction, typed JSON mapping, requests and previews with explicit branch-local dataflow.
**Architecture:** retain v2 executor/SQLite instances. Add bounded reusable data helpers, optional contracts and transient preview sessions; frontend binds JSON paths to step configuration.
**Tech Stack:** C11, json-c, libcurl, SQLite, native JS; Python/Node tests only.
**Spec:** docs/superpowers/specs/2026-11-04-json-dataflow.md

## Global constraints
- All source under src/, no new runtime dependencies; preserve existing rule and API behavior unless adding explicitly requested debug redaction.
- Reference/render/schema/transform depths32; values4MiB; transform arrays1000; ops32; preview sessions16, TTL600000ms, calls256.
- Do not commit, deploy, touch actual user databases, or undo concurrent changes.

## Review focus
- Missing/null/false/default must remain distinct; schema errors precede network IO.
- JSON Pointer escaping and string-valued URLs/body must roundtrip via UI.
- Preview tokens/Cookies stay usable server-side but absent from debug output.
- Fork/resume/retry must not double-map inputs or share mutable branch JSON.
- Rejected/stale preview results and resource-limit errors must not mutate session state.

## Tasks
- [x] Write failing C extraction/reference tests; implement nested extraction, bounded Pointer refs and reusable schemas/transforms. New rule/data.[ch], tests/test_data.c; modify rule/rule.c, rule/workflow.c, extract/extract.c, tests/test_core.c, tests/test_workflow.c, CMake.
- [x] Integrate input/schema/transform/response flags/retain_steps into engine/workflow.c. Add real-process JSON dataflow tests with HTTP count assertions, parallel contexts and POST payload types.
- [x] Implement transient engine/preview.[ch], debug redaction helpers, service lifecycle and /api/preview/step. Test temporary sessions, hidden secrets, preview nonpersistence and limits.
- [x] Add field binding trees, JSON forms, request dry rendering and continuous preview UI; extend Node tests before implementation.
- [x] Add runnable example and docs; independent review; run full Release and sanitizer suites, update evidence below.

## Helper interface ownership
Data helper task owns rule/data.[ch] and tests/test_data.c only. Exposes flow_schema_validate(schema,error,cap), flow_input_prepare(step,context,error,cap) mutating only its owned context on success, flow_transform_validate(step,error,cap), flow_transform(step,context,&output,error,cap), all int0/-1. It uses existing flow_render/flow_test/flow_clone.
Root owns extraction/reference/runtime/preview/web/CMake/integration. UI task owns web/ui/* and tests/test_workflow_ui.js. Interfaces follow spec above, frozen before independent work. Scope-authorized user implementation request supersedes additional skill permission handoffs.

## Execution ledger

- Initial RED: core rejected object extraction; typed $ref rendered as an ordinary object. Both passed after implementation. Reference lookup distinguishes explicit null (including array slots) from missing paths; native JSON strings and embedded NUL survive data operations, while HTTP text rejects NUL.
- Added real-service typed order flow: nested extraction → projection → transform → schema/defaults → POST JSON → emit. HTTP payload types, false/default values, original arbitrary field keys and pre-request schema failures are asserted.
- Group mapping RED exposed loss of prepared parent input on phase1 resume; wf_fork now atomically saves the prepared group context with children. Native null branch output also now aggregates successfully.
- Independent review reproduced composed reference depth overflow, optional exists reference failure, explicit-null preview input replacement, unrelated history array limits and secret provenance loss. Each fix received a RED→GREEN regression.
- Ruling: secret provenance is branch-local metadata, bounded to512 scalar values/1MiB, hidden from presentation. Track before input projection, after rendered requests, and conservatively through transforms; execution values remain original. Sensitive transform inputs/operation bindings may cause the complete transformed output to be hidden in debug views.
- Ruling: explicit POST /api/preview/release prevents repeated UI resets from occupying all16 temporary sessions until TTL expiry; busy/queued callers retain lifetime references and cannot reuse released sessions.
- No schema migration is required; SQLite format remains version3. All tests use isolated temporary data directories. No commits or deployed/user database changes.
- Final independent review re-ran the original secret-introducing map→pick→trim reproduction and confirmed output/context are redacted; all reported blocking findings resolved.
- Final Release:12/12 passed (23.43s). Final ASan/UBSan with detect_leaks=1 and halt_on_error=1:12/12 passed (23.71s). Default build refreshed; all build logs have no compiler warnings/errors. Node syntax/regressions and git diff --check pass.
- Firefox headless initial page load returned0; screenshot inspected at1440×1200 with connected service and new request/dataflow controls visible. This is a page-loading check, not a full browser interaction test; field-binding/session interactions are covered by Node and real HTTP integration tests.
