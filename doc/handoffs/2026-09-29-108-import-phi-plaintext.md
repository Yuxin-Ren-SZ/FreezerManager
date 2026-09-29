# Handoff note — 2026-09-29, CSV import splits PHI like every other write (#108, worker-2)

The first finding this session in the **disclosure** direction rather than the
data-loss direction. #71/#79/#83/#87 destroyed PHI its owner could not see; this
one **showed** it to callers who should never see it.

`ImportSamples` mapped a row's `custom_fields_json` CSV cell and stored it
verbatim. `SampleImport.cc` parsed the cell only to check it was a JSON object and
assigned the raw text; `commit_import()` inserted that row unchanged. So a cell
carrying a key the item type marks `is_phi` landed in `custom_fields_json` — the
**plaintext** column, which the `phi.read` gate does not cover. Any `sample.read`
holder received it from `GetSample`, and CSV export wrote it out. `commit_import`
bypassed `prepare_custom_fields`, which is where every other write path splits PHI
into `phi_fields_enc_json` and encrypts it.

**Reproduced before anything was changed**, in `tests/integration/sample_service_integration_test.cpp`
at `9943a5b` (the cell was `{"mrn":"MRN-555"}`, `mrn` being `is_phi` on the item
type):

```
ImportSamplesStoresPhiTaggedKeyEncryptedAtRest
  row->custom_fields_json.find("MRN-555")   -> 8     (expected npos)
  row->custom_fields_json.find("mrn")       -> 2     (expected npos)
  row->phi_fields_enc_json == "{}"                   (expected non-empty)
ImportSamplesHidesPhiTaggedKeyFromNonPhiReader        (Member: sample.read, not phi.read)
  as_member.custom_fields_json().find("mrn")     -> 2   (expected npos)
  as_member.custom_fields_json().find("MRN-555") -> 8   (expected npos)
```

The disclosure test also asserts the `phi.read` holder **does** see `mrn=MRN-555`,
so "hidden" cannot be satisfied by the value having been dropped. That is the
difference between proving non-disclosure and proving data loss.

**A second entry point, found while enumerating them:** `cli::run_sample_import`
(`freezerctl sample import`) inserts raw rows straight into the **same database the
server serves**, with no split and no KMS. Its plaintext row is disclosed over gRPC
exactly like the server's, and it is the path an operator drives by hand. It
reproduced too (`7d65368`), and is fixed here rather than filed.

**Changed:**

- **New `src/storage/CustomFieldWrite.h`** — `prepare_custom_fields`,
  `PreparedCustomFields` and `is_blank_phi_value` **moved** out of the anonymous
  namespace in `src/server/SampleServiceImpl.cc`, header-only and inline like the
  neighbouring `CustomFieldResolver.h`/`SampleOps.h`. The body is #83's per-key-merge
  version verbatim; the only semantic change inside it is the missing-KMS error,
  which now names the PHI keys it could not store instead of saying only that
  something is misconfigured.
- `src/server/SampleServiceImpl.cc` — `CreateSample` and `UpdateSample` call the
  moved function. `commit_import()` now validates and splits each row inside its
  transaction; a row that cannot be split safely throws before anything is written,
  so the whole batch rolls back under the existing all-or-nothing contract.
  `report_import_validation()`'s dry-run probe runs the same split.
- `src/cli/SampleCommands.{h,cc}` — `SampleImportOptions` gains
  `const kms::IKmsProvider* kms` (nullptr = none configured). `run_sample_import`
  prepares every row through the same function, for the real import and for
  `--dry-run`; a failing row is reported by number and the transaction is never
  committed. `src/cli/CliApp.cc` passes `kms::make_default_kms()`.
- `src/storage/CMakeLists.txt` — the `storage` INTERFACE target links
  `FreezerManager::crypto` and `FreezerManager::kms`, which the new header uses.

**Entry-point audit** (the issue asked for this, and it is why the CLI path is in
scope):

| Path | Writes `custom_fields_json` | Splits PHI |
|---|---|---|
| `SampleServiceImpl::CreateSample` | yes | yes |
| `SampleServiceImpl::UpdateSample` | yes | yes (#83) |
| `SampleServiceImpl::commit_import` (gRPC + REST `/api/v1/sample/import`) | yes | **was no — fixed** |
| `cli::run_sample_import` (`freezerctl sample import`) | same database | **was no — fixed** |
| `SampleServiceImpl::report_import_validation` (dry-run probe) | inserts, never commits | **was no — fixed** |
| `cli::rotate_phi_keys` | rewraps the envelope only; no custom-field input | n/a |
| `SampleOps::move_sample` / `apply_checkout` | no custom-field input; preserve the columns | n/a |

**Decisions:**

- **Home: `src/storage/`, argued from the invariant, not from the link list.** What
  this function decides is what lands in one row's two custom-field columns, and its
  inputs are things this layer already reads: `resolve_custom_field_defs` walks the
  item-type taxonomy **from the caller's own transaction**, exactly as it does one
  header over. `crypto::encrypt` is reached through the caller's `IKmsProvider`, an
  interface, so §5 ("stay behind interfaces") holds.
- **The pure-`src/core/` alternative was evaluated first and rejected.** Splitting
  `definitions + blob -> (non-PHI blob, PHI map, flags)` in `src/core/` keeps that
  module clean, but it leaves the **lab-PHI-mode check and the missing-KMS refusal**
  at each call site — two security decisions duplicated per writer, which is the
  shape of the bug being fixed. A shared helper that is slightly mis-layered beats
  three copies of a security decision; the shared function is what makes the four
  writers agree by construction.
- **A PHI-carrying row with no master key is refused, not stored.** With no KEK the
  value cannot be encrypted, and the alternative is the disclosure this fix exists to
  prevent. The error names the keys: `no master key is configured; cannot store PHI
  custom fields: [mrn]`. **Remedy:** configure `FMGR_MASTER_KEK` (or
  `CREDENTIALS_DIRECTORY/master_kek`) for the `freezerctl` invocation — the same KEK
  the server uses, without which the row would not be readable by the server anyway.
  Pinned on both sides: the refusal (`ImportRefusesPhiTaggedKeyWithoutMasterKey`) and
  its mirror, that an import carrying **no** PHI-tagged key still succeeds with no KEK
  configured (`ImportWithoutPhiTaggedKeysNeedsNoMasterKey`), so ordinary bulk loads
  are unaffected.
- **The dry-run probe runs the same split.** Without it this fix would *create* a
  divergence: the probe would report a row OK that `commit_import` now refuses. It
  narrows #110 (the probe and the commit path now decide PHI and custom-field
  validity identically); #110's own criterion — one file through both paths with the
  per-row outcomes asserted to match — is not written here and remains open, as does
  any non-PHI drift.
- **The import now validates custom fields**, because that is part of the function it
  reuses. A known field with the wrong type is refused (`INVALID_ARGUMENT`, nothing
  persisted, pinned by `ImportSamplesRejectsCustomFieldFailingValidation`) where it
  was previously stored unchecked. Unknown keys still pass, so an import that used to
  work does not start failing for a key with no definition — pinned by the untagged
  `strain` assertion in the stored-columns test.

**Tests** (`tests/integration/sample_service_integration_test.cpp`,
`tests/unit/cli_test.cpp`) — seven, each verified red against the code path it
pins, plus one regression pin that was green on arrival:

| Test | Red evidence |
|---|---|
| `ImportSamplesStoresPhiTaggedKeyEncryptedAtRest` | red at `9943a5b` (reproduction commit) |
| `ImportSamplesHidesPhiTaggedKeyFromNonPhiReader` | red at `9943a5b` |
| `ImportSamplesRejectsCustomFieldFailingValidation` | red with `commit_import`'s split reverted: `status.ok()` was `true` |
| `ImportSamplesDryRunReportsCustomFieldValidationFailure` | red with the probe's split reverted: `resp.rows(0).ok()` was `true` |
| `CliBackendTest.ImportStoresPhiTaggedKeyEncryptedAtRest` | red at `7d65368` |
| `CliBackendTest.ImportRefusesPhiTaggedKeyWithoutMasterKey` | red at `7d65368` |
| `CliBackendTest.ImportDryRunRefusesPhiTaggedKeyWithoutMasterKey` | red with the CLI dry-run split reverted: exit code 0, not 1 |
| `CliBackendTest.ImportWithoutPhiTaggedKeysNeedsNoMasterKey` | green on arrival (pin) |

The last three rows are not "asserted red" — the split was **removed again** from
`commit_import`, from the dry-run probe and from the CLI dry run in a throwaway
edit, the tests were shown to fail with the output above, and the files were
restored byte-for-byte (`git status` clean apart from this note). Plant the
violation before claiming the test catches it.

**Verification** (rebased on `origin/main` = `55016d6`, so #83's per-key merge is in
the tree):

```
$ cmake --build --preset dev
$ ctest --preset dev
100% tests passed out of 1549
Total Test time (real) = 155.04 sec
```

Focused, after the fix: `freezermanager_sample_service_integration_tests`
**69/69 passed**; `freezermanager_cli_unit_tests` **171 ran, 131 passed, 40
skipped, 0 failed**.

`tools/check-spdx-headers.sh` passes and `clang-format --dry-run --Werror` (17.0.6)
is clean on every changed file.

**Known limitations / follow-ups:**

- **clang-tidy was not run locally in any way that counts.** On this machine it
  cannot parse this SDK's libc++ for `src/cli/SampleCommands.cc`
  (`clang-diagnostic-error` on `<__algorithm/sort.h>`), and where it does run it is
  a **superset** of CI: it reports `readability-convert-member-functions-to-static`
  and `bugprone-empty-catch` on lines that main's green CI passes. Linting
  `src/server/SampleServiceImpl.cc` locally reported **zero findings in the new
  header**, which is the only signal taken from it. CI's standalone `clang-tidy` job
  is the verdict; no lint coverage is claimed here.
- **The Postgres legs were skipped locally** (`FMGR_TEST_POSTGRES_URL` unset): 40 CLI
  and 40 backend-conformance tests. CI runs them. The SQLite leg of every new test
  passed.
- **The import path validates now**, which is a behaviour change beyond PHI: a
  previously-accepted row with a wrongly-typed value for a **defined** field is now
  refused with `INVALID_ARGUMENT`. Called out above so it is a decision, not a
  surprise.
- **`core/custom_field_validator.h:134` ignores a `[[nodiscard]]` result**
  (`Uuid::parse`), a pre-existing warning that this change makes visible in one more
  translation unit (`SampleCommands.cc`). Not fixed here: it is outside this issue
  and the warning is not an error in any preset. `-Werror` is not set repo-wide.
- **#110 stays open.** The probe change narrows the divergence; the agreement test
  its acceptance criteria require is not written here.
- **A `phi.read` holder's request remains authoritative for the whole envelope**
  (#79's decision): that is #83's boundary, untouched by this change.
