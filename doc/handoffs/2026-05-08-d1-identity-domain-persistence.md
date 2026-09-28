# Handoff note — D1 identity domain persistence

Implemented D1.1 and D1.2 for `Lab`, `User`, and `LabMembership`:
core identity types, storage traits, SQLite `0002_identity` migration,
production SQLite repositories, case-insensitive user email uniqueness,
foreign-key-backed memberships, soft-delete visibility, and focused unit
coverage. `Lab` soft-delete uses `archived_at_micros`; `User` soft-delete
sets `disabled`; `LabMembership` soft-delete sets `revoked_at_micros`.

D1.3 remains deferred: the initial `SystemAdmin` first-run wizard should land
after D2 provides role/permission tables, or with K5 once CLI bootstrap,
auth, KMS, and TLS setup exist.
