# Schema migrations

Each backend applies an ordered list of schema migrations. A migration is a
`(version, name, up_sql)` triple; its integrity is protected by a checksum
(`blake2b("version:name:up_sql")`) stored in the `schema_migrations` table. On
startup the backend re-checksums each already-applied migration and **aborts** if
a checksum changed — so the SQL body of an applied migration must never change.

## Where migrations live

SQL bodies are one file per migration:

- SQLite: `src/storage/sqlite/migrations/NNNN_name.sql`
- Postgres: `src/storage/postgres/migrations/NNNN_name.sql`

At configure time CMake runs `tools/gen_migrations_header.py` to embed those files
into generated headers (`SqliteMigrationsEmbedded.h`, `PostgresMigrationsEmbedded.h`)
as `constexpr` string views — there is no runtime file I/O, so `:memory:` and
packaged binaries work unchanged. `default_migrations()` in each backend builds
its list from the embedded array.

The generated headers are build artifacts (git-ignored, regenerated on every CMake
configure), so the `.sql` files are the reviewable source of truth.
`tests/unit/migration_files_test.cpp` asserts the embedded bodies match the `.sql`
files and that every checksum equals a pinned golden value.

## Adding a migration

1. Create `NNNN_name.sql` in the backend's `migrations/` directory, where `NNNN`
   is the next zero-padded version and `name` matches the filename stem. Add the
   matching Postgres file if the change applies to both backends.
2. Re-run CMake configure (or `python3 tools/gen_migrations_header.py <dir> <header> <array_var>`)
   to regenerate the embedded header.
3. Add the new migration's golden checksum to `migration_files_test.cpp`
   (the test prints the computed value on mismatch; or compute it with
   `printf '%s:%s:%s' <version> <name> "$(cat file.sql)" | b2sum -l 256`).
4. **Never** edit an already-released migration file — write a new one.

## Notes

- The array is sorted by version at build time; source order does not matter.
- SQLite migration 0001 was a placeholder later replaced by 0011; Postgres 0001
  already had the full audit schema, so its 0011 is a no-op. Do not "fix" this —
  changing either would break the checksum of a released migration.
