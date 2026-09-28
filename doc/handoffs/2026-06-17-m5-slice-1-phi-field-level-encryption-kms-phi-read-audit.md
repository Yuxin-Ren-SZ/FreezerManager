# Handoff note — 2026-06-17, M5 slice 1 (PHI field-level encryption + KMS + PHI-read audit)

First PHI-safety slice (PRD §8). PHI custom fields stop sitting in plaintext: they
are split out of the sample's plaintext blob, AEAD-encrypted with a per-record DEK
wrapped by a master KEK, and disclosed on read only to `phi.read` holders — every
disclosure audited. Scope: Sample entity only.

**New modules:**
- `src/kms/IKmsProvider.h` — envelope KMS interface: `wrap_dek`/`unwrap_dek`
  (`WrappedDek{nonce,ciphertext}`) + `key_id()`. Domain-free, I/O-free.
- `src/kms/EnvVarKms.{h,cc}` — dev/test provider; KEK from `FMGR_MASTER_KEK`
  (base64, 32 bytes), `crypto_secretbox` DEK wrapping, fail-fast on missing/short
  key. `key_id()` = BLAKE2b fingerprint. **NOT for production** (OsKeyring/Vault
  later). `tests/unit/kms_test.cpp` (9): round-trip, fresh nonce, tamper/wrong-key
  rejected, bad/missing key fail-fast.
- `src/crypto/FieldCipher.{h,cc}` — `encrypt(PhiFields, kms)`/`decrypt(...)`.
  Fresh per-record DEK, per-field `crypto_secretbox`. Envelope JSON
  `{v,kek_id,dek:{n,c},fields:{key:{n,c}}}`; empty map → `"{}"`.
  `tests/unit/field_cipher_test.cpp` (9): round-trip, mixed types, plaintext never
  in envelope, fresh-DEK divergence, tamper/wrong-KEK/malformed rejected.

**Audit seam:**
- `ITransaction::note_phi_read(entity_kind, entity_id, ctx, field_keys)` — new
  virtual, default throws `UnsupportedOperation`. Sqlite/Postgres transactions
  override it to append an immutable `action="phi.read"` row (joins the same hash
  chain) with `after_json={"phi_keys":[...]}` — **key names only, never values**
  (PRD §7.3, §17). Delegates to `note_mutation`.

**Service wiring:**
- `SampleServiceImpl` takes a borrowed `kms::IKmsProvider*` (nullable). Write
  (`Create`/`Update`): `prepare_custom_fields` resolves defs, validates the combined
  blob, splits by `is_phi` — non-PHI stays plaintext, PHI encrypted into
  `phi_fields_enc_json`. PHI write requires `Lab.is_phi_enabled` (else
  `INVALID_ARGUMENT`) + a configured KMS (else `INTERNAL`); does **not** require
  `phi.read`. Read (`Get`/`List`): `reveal_phi` decrypts + merges into
  `custom_fields_json` only when the caller holds `phi.read`, then `note_phi_read`
  per disclosing sample. `validate_sample_custom_fields` removed (folded into
  `prepare_custom_fields`).
- `FreezerServer` builds `EnvVarKms` from env at construction; null (PHI disabled,
  warn-logged) when `FMGR_MASTER_KEK` unset. `kms_` declared before `sample_svc_`.
- `ItemTypeServiceImpl` rejects `is_phi ∧ indexed` on CFD create/update (PRD §4.1).
- `tests/integration/sample_service_integration_test.cpp` (+5): ciphertext at rest,
  value absent from both columns, visible to phi.read holder, hidden from non-holder,
  write without phi.read, PHI-read audit row keys-only. Fixture now sets
  `FMGR_MASTER_KEK`, enables PHI on the labs, grants `phi.read` to SystemAdmin
  (excluded by default per PRD §3), and seeds a non-required `is_phi` CFD.
  `item_type_service_integration_test.cpp` (+1): indexed-PHI rejected.

**Known limitations / follow-ups:**
- PHI on Sample only; other entities have no PHI column yet.
- List discloses one `phi.read` audit row per sample (precise but verbose for large
  lists); per-request batching is a later option.
- `EnvVarKms` only; OsKeyringKms / VaultKms and `key.rotate` rotation deferred.
  `kek_id` is recorded in the envelope for forward rotation support.
- Decrypted PHI is **not** echoed in the Create/Update response (only on Get/List
  for phi.read holders).

Verification: `cmake --build --preset dev` clean; `ctest --preset dev -j1` — new
kms/crypto/sample-PHI/item-type tests green; only the 12 pre-existing baseline
failures remain (SqliteBackend file-detection, CustomFieldResolver, E2E unauth).
`run-clang-tidy-17` on new/changed sources clean; `clang-format-17` clean;
`tools/check-spdx-headers.sh` clean; `git diff --check` clean. Postgres
`note_phi_read` override compiles but is untested without `FMGR_TEST_POSTGRES_URL`.
