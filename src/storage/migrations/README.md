# Schema migrations

Schema and migrations are embedded in `src/storage/storage.c`; the current
`PRAGMA user_version` is **3**. Startup rejects a newer unsupported version
without rewriting it. Migrations run before workers access the Store.

## Upgrade to v3

The first startup with a pre-v3 database performs a transactional conversion
of each current v1 rule definition in `spiders` into an ordinary v2 workflow.
It increments that spider's rule version and appends the converted definition
to `rule_versions`. Existing workflow definitions are retained. The workflow executor accepts v2 only; there is no parallel v1 execution
mode. The product Web is read-only; workflow create/update/preview operations
are available only in the tests-only engine fixture.

Historical rule versions, run snapshots, items, artifact records, and IDs are
preserved. Unfinished legacy runs are marked `failed` with an explicit migration
message and are not replayed. Their snapshots are not rewritten. Invalid legacy
JSON or a definition that cannot be converted aborts the migration transaction,
rolls back the upgrade, and causes service startup to fail; it is not silently
skipped or replaced.

Conversion generates ordinary request/foreach/condition/download/emit/join steps.
Literal legacy request values are stored in `vars`, which are not recursively
template-expanded. Pagination caps and request policies are represented by
workflow steps. Converted rules remain subject to workflow limits, including
1000 elements per foreach and at most 10000 created step instances per run;
conversion does not promise unrestricted execution of formerly larger runs.

The earlier v2 migration transactionally rebuilds requests/items, preserves
legacy IDs and results, adds workflow metadata and partial unique indexes, and
creates persistent `step_instances` and `sessions`. When starting from an older
database, required schema changes and v3 definition conversion are committed
as one startup upgrade. Foreign keys are disabled only during migration, before
workers can access the Store.

Stop the service and back up the complete data directory before upgrading,
including registered downloads. There is no downgrade or old-binary rollback
compatibility promise for an upgraded database. Do not restore only the main
`.db` file from a running WAL database; use an offline copy of the complete
data directory or an appropriate SQLite backup procedure. Future migrations
must continue preserving historical snapshots and records.

## Code-owned task catalog

The additive `code_spiders` table is created by the existing startup schema
transaction; core `user_version` remains 3. It maps stable code IDs to persistent
spider IDs and records membership in the currently compiled task catalog.

Before workers start, `crawler_sync` applies all code tasks in one transaction.
Unchanged configurations preserve version and `next_run`; changes append a
version. Removed/legacy tasks are disabled and their unfinished runs cancelled,
while versions, completed runs, results and files remain. Re-adding a stable
code ID restores the same spider ID. Failed sync rolls back catalog membership,
configuration versions and lifecycle changes together.
