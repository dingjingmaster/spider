# Code Crawlers Status Dashboard Implementation Plan

> Execute inline under the user's explicit product correction; no additional approval or commits.

**Goal:** Code declares executable crawler tasks, while the Web only displays their status and history.
**Architecture:** crawler_add/crawler_sync normalize code task definitions into a stable SQLite catalog; scheduler consumes the resulting persistent jobs. Web depends only on Status and has GET-only routes.
**Tech Stack:** Existing C11/POSIX, SQLite/json-c, libcurl, CivetWeb, vanilla JS.
**Spec:** docs/superpowers/specs/2026-11-11-code-crawlers-status-design.md

## Global Constraints

Preserve the existing uncommitted work. No actual site invented, no real user data touched, no commit/push. New module functionality remains behind each module's single public header. Current source registration is authoritative; historic data stays available in SQLite without scheduling removed/legacy tasks.

## Review Focus

- Stable code key must preserve IDs/history and unchanged next_run across restart.
- Partial/invalid sync must roll back every catalog/config/schedule change.
- Removed or legacy tasks must never execute from restored queued runs.
- All mutation endpoints must be rejected even if called directly, not merely hidden in UI.
- Status responses must not leak configuration snapshots or credentials; late UI polls must not overwrite the selected crawler/run.

## Tasks

- [ ] Add GET-only regression against current production binary; observe rejection mismatch.
- [ ] Add crawler_add/crawler_sync and atomic code catalog persistence; verify stable identity, removal, rollback and legacy quarantine.
- [ ] Replace production Web with Status-only GET routes and a dashboard; preserve engine regression controls solely in a tests-only executable.
- [ ] Verify real success/failure scheduled code tasks, restart, old-task exclusion, credential omission and UI behavior.
- [ ] Update docs; review; run complete Release and ASan/UBSan suites plus diff/syntax checks.

## Ledger

- Ruling: the user's explicit requirement fully specifies a read-only status product; no further UI permission or planning handoff is needed.
- Ruling: keep the generic workflow implementation as an available engine, but register no fake business tasks. Empty code catalog is a valid first launch.
