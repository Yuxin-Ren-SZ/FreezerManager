# Handoff note — 2026-06-17, M5 slice 2 (production KMS + key rotation)

Hardens the PHI key story from dev-only to production-ready and adds master-KEK
rotation (PRD §8). Builds on slice 1. Trigger is the CLI (the new KEK is staged
in the server's credential store, never sent over an RPC); a `RotateKeys` RPC can
wrap the same engine later.

**Keyring + key sources (`src/kms/`):**
- `IKmsProvider::unwrap_dek` now takes the envelope's `kek_id` so a provider can
  hold several KEKs. `KeyringKms` is the shared engine: a `map<kek_id, key>` with
  one active KEK; `wrap_dek` seals under the active KEK, `unwrap_dek(w, kek_id)`
  selects by id (throws on unknown id). Centralizes the `crypto_secretbox`
  wrap/unwrap + BLAKE2b fingerprint that used to live in `EnvVarKms`.
- `EnvVarKms` is now a `KeyringKms` loader: `FMGR_MASTER_KEK` (active) +
  optional `FMGR_MASTER_KEK_PREVIOUS` (comma-separated base64 retired keys).
- `OsKeyringKms` (production): reads `$CREDENTIALS_DIRECTORY/master_kek` (active)
  and `master_kek.prev.*` (retired) — systemd-creds `LoadCredential=`. Each file
  is raw 32 bytes or base64. Pure file I/O + libsodium, no new dependency.
- `make_default_kms()` (`KmsFactory`): OS keyring if `$CREDENTIALS_DIRECTORY/
  master_kek` exists, else `EnvVarKms`, else null (PHI disabled). `FreezerServer`
  and the CLI both use it.

**Rotation:**
- `crypto::rewrap(envelope, kms)` re-wraps the per-record DEK under the active
  KEK and rewrites `kek_id`; field ciphertext is copied verbatim (DEK unchanged),
  so no plaintext PHI is decrypted to disk. Returns nullopt for an empty envelope
  or one already on the active KEK.
- `cli::rotate_phi_keys(backend, kms, lab?, actor, sink)` walks samples
  (incl. tombstoned — PHI persists) per lab, rewraps, and updates via the Sample
  repo. Returns `{scanned, rewrapped, current, failed}`. A sample that fails to
  re-wrap (e.g. update FK re-validation) is counted in `failed` and logged, not
  aborted — so the old KEK is retired only once `failed == 0`.
- `freezerctl key rotate [--sqlite|--postgres] [--lab <uuid>] --actor <uuid>`.

**Operator runbook (rotate the master KEK):**
1. Move the current KEK to `FMGR_MASTER_KEK_PREVIOUS` (or a `master_kek.prev.*`
   credential file).
2. Install the new KEK as `FMGR_MASTER_KEK` / `master_kek`.
3. Run `freezerctl key rotate --actor <uuid>` until it reports `failed 0` and
   `rewrapped + current == scanned`.
4. Drop the retired KEK once every record is migrated.

**Tests:** `kms_test` (keyring: retired-key unwrap, unknown-id throws, env
previous keys), `os_keyring_test` (base64/raw files, retired keys, missing
dir/file fail-fast), `field_cipher_test` (rewrap rotates + still decrypts, no-op
when current/empty), `cli_test` (rotate rewraps to active KEK + audits + second
rotate no-op; argv `key rotate` via env). SQLite + Postgres-param where relevant.

**Known limitations / follow-ups:**
- Rotation audits as a Sample `update` (after-image shows the new `kek_id`,
  ciphertext only). A distinct `key.rotate` audit *action* would need a custom
  action threaded through the repository update path (the `MutationContext.reason`
  is currently not persisted) — deferred. The `key.rotate` permission and the
  online `RotateKeys` RPC are also deferred.
- Rotation scope is the Sample entity (the only PHI column).
- `VaultKms` (transit engine) not implemented.

Verification: `cmake --build --preset dev` clean; `ctest --preset dev -j1` — new
kms/crypto/os-keyring/cli-rotation tests green; only the 12 pre-existing baseline
failures (SqliteBackend file-detection, CustomFieldResolver, E2E unauth) plus the
known-flaky `FuzzRateLimiter` under parallel load (passes in isolation).
`run-clang-tidy-17` on new/changed sources clean; `clang-format-17` clean;
`tools/check-spdx-headers.sh` clean; `git diff --check` clean.
