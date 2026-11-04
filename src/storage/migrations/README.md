# Schema migrations

Initial schema is embedded in `storage.c` and identified with SQLite
`PRAGMA user_version = 1`. A newer, unsupported user_version is rejected
without rewriting it. Future numbered migrations belong in this folder,
must run transactionally, and must preserve historical rule snapshots.
