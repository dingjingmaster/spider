# Modular Crawlers Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans inline; the user has authorized implementation. Execute all tasks without a further handoff or commit.

**Goal:** Public module boundaries, registered spiders, and parallel crawlers with strictly serial operations within each crawler.
**Architecture:** Opaque module handles and semantic storage APIs; a crawler registry dispatches native C and the existing workflow adapter. A scheduler owns full runs and retains exclusion through callback teardown.
**Tech Stack:** C11/POSIX, libcurl >= 7.85, SQLite >= 3.35, json-c >= 0.15, Lexbor, CivetWeb.
**Spec:** docs/superpowers/specs/2026-11-11-modular-crawlers-design.md

## Global Constraints

All product/test/vendor source stays in src/. Each module has one public .h/.c; helper files remain private. Preserve JSON workflow API, schema version 3, committed checkpoints, POST replay boundary, and file rollback ownership. No commits or pushes. Work in the current clean checkout as requested.

## Review Focus

- Cancellation returns before the worker exits: the spider remains excluded until teardown.
- Workflow fanout: every request in one run is serial, multiple spiders still overlap.
- Shutdown during HTTP or backoff: wake/abort promptly and preserve checkpoints.
- File registration failure: rollback cannot remove another download's registered file.
- Invalid/unknown implementation and edited config: fail early, use the stored run snapshot.

### Task 1: Establish baseline and concurrency regression

- [x] Build Release and run all existing tests.
- [x] Add src/tests/scheduler_integration.py with a real HTTP server measuring active requests per crawler and globally; assert max_per_crawler == 1 and max_global >= 2.
- [x] Run against baseline and observe failure for same-crawler overlap.

### Task 2: Public modules and persistence boundaries

- [x] Create crawler, download, parser, status interfaces; move site-independent helpers into private files.
- [x] Make Store opaque in storage.h; move SQL from workflow HTTP recording, cookies, Web list/export into semantic storage APIs.
- [x] Keep download independent from persistence using a file-registration callback and independent cookie serialization.
- [x] Update existing low-level tests to new public APIs or explicitly private storage fixtures and run their tests.

### Task 3: Registered spiders and full-run scheduling

- [x] Create crawler registry and per-run context; register workflow through spiders/spiders.c.
- [x] Move workflow under spiders/workflow/private, consolidate rule into rule.h/c with private helpers, use parser/download/storage public interfaces.
- [x] Implement fixed workers and timed controller; guard spider IDs through run teardown, route manual triggers/stop through scheduler.
- [x] Add native C registry/scheduler tests, including blocked cancellation cleanup, unknown definitions, different-crawler overlap and duplicate rejection.
- [x] Run scheduler integration regression to green and all existing recovery/preview tests.

### Task 4: Web, documentation and validation

- [x] Wire Web to module APIs and status; preserve existing endpoint behavior and add implementation selection for native registrations.
- [x] Document module dependencies, ownership, registration example and serial concurrency semantics; update stale README/workflow statements.
- [x] Build and run full Release and ASan/UBSan suites, check warnings and git diff --check.
- [x] Review dependency leaks and cancellation/shutdown/file rollback boundaries; fix and reverify affected cases.

## Execution ledger

- Ruling: work directly in the requested current project and preserve uncommitted diff for review; no worktree/commit handoff.
- Ruling: the user authorized the proposed design, including retaining JSON workflow as the default registered implementation; proceed without another approval request.

- Baseline: 12/12 Release tests passed outside the initial socket-restricted sandbox. New concurrency test failed on baseline with A=4/B=4 simultaneous requests.
- Tasks 1-3: complete. First full refactor suite passed 14/14 (22.91s), including native C callback teardown and real HTTP per-spider serial execution.
- Task 4: implementation and docs complete; final reviewer and sanitizer verification pending.
- Ruling: retain rule as an independent facade because workflow validation and SQLite legacy migration both use it; avoid storage depending on a concrete spider.
- Native config editor RED: workflow-only validation rejected native config; GREEN: native config roundtrip preserves arbitrary fields, workflow editor/debug safely hides.
- Native storage RED: cancelled emit reported success; GREEN: validate active run in the same transaction, preserve idempotence and counter rollback.

- Final review: both Important findings reproduced and fixed. Native CSV now infers result columns independently of site-specific fields; public transport defaults origin to the separately supplied URL when options.url is absent.
- Transport RED/GREEN: real HTTP fixture checks Authorization forwarding, POST body, explicit headers-origin suppression and explicit body-origin blocking.
- Final Release: 15/15 passed in 23.91s. Final ASan/UBSan: 15/15 passed in 23.15s with detect_leaks=1 and halt_on_error=1. Builds have no warnings/errors; Node syntax and git diff --check passed.
- Review minor deferred: registry userdata remains preview/destroy-owned; native run callbacks use their independent local/per-run state. No registered shared mutable state is required by the delivered implementations.
- Task 4: complete. Changes remain uncommitted for user review; no target-website crawling or live database migration performed.
