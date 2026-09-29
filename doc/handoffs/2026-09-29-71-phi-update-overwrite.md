# Handoff note — 2026-09-29, PHI survives an edit by a caller without `phi.read` (#71, worker-1)

`SampleServiceImpl::UpdateSample` used to write the prepared PHI envelope over the
stored one unconditionally, and `prepare_custom_fields()` defaults that envelope to
`"{}"`. A caller without `phi.read` never receives the PHI fields — `reveal_phi()`
returns early and leaves them out of the response — so editing any unrelated field
replaced `phi_fields_enc_json` with `"{}"`: the sample's PHI was destroyed silently,
permanently, and the response looked like a normal successful edit. In a biospecimen
manager that is the worst class of bug this codebase can have (PRD §8), because
nothing surfaces it. The bug was **pre-existing**, not introduced by the SPA: the
unconditional assignment and the `reveal_phi` gate both landed in `284e98b`
(*feat(server): encrypt PHI sample fields on write, gate + audit on read*,
2026-06-17, the M5 PHI slice), an ancestor of `main`. The released paths were the
gRPC `SampleService.UpdateSample` RPC and REST `POST /api/v1/sample/update`; G3.3's
edit form (#59) is only the newest caller, and the one that made the bug reachable
in practice.

**Changed:** `src/server/SampleServiceImpl.cc` — `PreparedCustomFields` gained
`phi_keys_present`, set in `prepare_custom_fields` when the incoming blob carries a
PHI-tagged key; `UpdateSample` now rewrites `phi_fields_enc_json` only when
`prepared.phi_keys_present || (kms_ != nullptr && sctx.has_for_lab(lab_id,
Permission::PhiRead))`. The second clause is `reveal_phi()`'s disclosure condition —
see the second decision below.
`CreateSample` is unchanged apart from a comment, because a new row has no stored
envelope to protect. `tests/integration/sample_service_integration_test.cpp` — five
tests plus fixture helpers `update_sample`, `stored_phi_envelope` (raw column),
`stored_phi` (decrypted with the same dev KEK the server loads), `get_sample` and
`custom_fields`. No change to `src/crypto/` or `src/kms/`: the fix only decides
*whether* the envelope is overwritten, never how it is encrypted.

**Decisions:**

- **The rule is "the request is authoritative for PHI, or it says nothing".** A
  `phi.read` holder saw the fields, so their request decides the envelope exactly as
  before — including an update that carries no PHI key at all, which deliberately
  clears it. Everyone else's request cannot express PHI, so the stored envelope is
  left untouched. The two cases are told apart from `phi_keys_present` plus the
  caller's permission, not by guessing from an empty envelope — `"{}"` is genuinely
  ambiguous between "no PHI was sent" and "PHI is empty".
- **The permission alone is not the whole condition: it also needs a KMS.** A server
  started with no master KEK (`FMGR_MASTER_KEK` unset, no keyring credential —
  `make_default_kms()` returns null and `kms_` is null) still holds PHI rows, and
  `reveal_phi()` returns early for everyone in that state. So a `phi.read` holder has
  *not* seen the fields there either, and the first version of this fix still let
  their unrelated edit wipe the envelope — the same bug, second trigger. The
  condition now mirrors `reveal_phi()`'s: `kms_ != nullptr && holds phi.read`. Pinned
  by `UpdateSampleWithoutConfiguredKmsPreservesStoredPhi`, red before the clause.
- **A request that *does* carry PHI keys is still honored without `phi.read`.** PHI
  write has never required PHI read (`PhiWriteDoesNotRequirePhiRead` pins that for
  create), and silently discarding supplied values would be the same class of data
  loss this fix removes. The behaviour is now pinned by a test for update too.
  Rejecting a non-reader's PHI keys outright would be a permission-model change and
  does not belong in this issue.
- **Other writers of the column were checked, as the issue asked.** `KeyCommands.cc`
  key rotation re-reads each row inside its transaction and re-wraps the stored
  envelope, preserving every field — nothing to fix. `ImportSamples` only inserts.
  The generic repository `update` is reached only from `UpdateSample` and rotation.
- **No sanitizer run.** The issue asks for `asan` only if encryption or decryption is
  touched; it is not, so the full `dev` suite is the evidence instead.

**Tests:** `tests/integration/sample_service_integration_test.cpp`:

- `UpdateSampleByNonPhiReaderPreservesStoredPhi` — the decisive test. Seed PHI as a
  `phi.read` holder, edit the name as a member without `phi.read` sending back
  exactly the custom fields it was shown, then read it back as the holder. Asserts
  the raw column is not `"{}"`, that the value still decrypts, and that the plaintext
  column never gained it.
- `UpdateSampleWithoutConfiguredKmsPreservesStoredPhi` — the same edit against a
  second listener built with no master KEK; the envelope must survive. Red before the
  KMS clause, with the same `actual: "{}"` failure.
- `UpdateSampleByPhiReaderReplacesStoredPhi` and
  `UpdateSampleByPhiReaderWithoutPhiKeysClearsStoredPhi` — the holder semantics that
  must not regress.
- `UpdateSampleByNonPhiReaderWhoSuppliesPhiStoresIt` — explicit PHI keys are stored.

Red first (`9b20e0b`): `ctest --preset dev -R 'UpdateSampleBy' --output-on-failure`
→ `75% tests passed, 1 tests failed out of 4`, with
`Expected: (*envelope) != ("{}"), actual: "{}" vs "{}"` and
`C++ exception with description "map::at:  key not found"`. The KMS test was red on
its own before its clause: `ctest --preset dev -R UpdateSampleWithoutConfiguredKms`
→ `0% tests passed, 1 tests failed out of 1`. After the fix:
`100% tests passed out of 5`; `-R 'UpdateSampleBy|UpdateSampleWithoutConfiguredKms|Phi|SampleService'`
→ `65/65` (2 Postgres tests skipped, `FMGR_TEST_POSTGRES_URL` unset locally); full
`ctest --preset dev` → `100% tests passed out of 1446`, exit 0, 222 skipped
(Postgres and the parameterized backend suites). Repo-wide
`clang-format --dry-run --Werror` and `tools/check-spdx-headers.sh` are clean.

`run-clang-tidy-17` is not installed on the owner's Mac, so `clang-tidy` 17.0.1 from
the shared `.venv` was run per file with `--warnings-as-errors='*'`, A/B against the
`origin/main` versions of both files. It is a hint rather than a verdict here: the
local compile DB is AppleClang, and clang-tidy 17 cannot parse that SDK's libc++
(`__builtin_clzg`), which makes it emit `clang-diagnostic-error` plus bogus
`readability-convert-member-functions-to-static` /
`readability-implicit-bool-conversion` findings on lines `main`'s green CI passes.
Filtering those artifacts, the two versions are identical — with one exception found
and fixed: the new `get_sample` helper tripped a genuine
`bugprone-easily-swappable-parameters`, silenced with the same
`NOLINTNEXTLINE(bugprone-easily-swappable-parameters)` the fixture's `login` already
uses.

Two `GrpcTlsTest` crashes (`WrongHostnameRejected` SIGTRAP, `EcdsaKeyWorks` SEGFAULT)
appeared in intermediate full runs **while a clang-tidy A/B sweep was running
concurrently on the same machine**. They are load artifacts, not this change: the TLS
binary's **18** tests all pass when it is run alone (`--gtest_list_tests` and
`ctest -N` both report 18; an earlier revision of this note said "5/5", which is not
this binary's test count whichever run produced it — the conclusion stands, but
dismissing a SEGFAULT should not rest on a figure that does not check out), nothing
in this diff is reachable from it, and a final full run with the machine otherwise
idle is `1446/1446`, exit 0. Worth knowing before chasing that SIGSEGV in a review.

**Known limitations / follow-ups:**

- A `phi.read` holder whose client does not round-trip the PHI fields still clears
  the envelope on update. That is the documented semantic ("an absent PHI key means
  cleared") and was deliberately preserved. Making an update partial would need a
  field mask in the proto — a `lock:proto` change, out of scope here.
- While no master KEK is wired, a `phi.read` holder's update is never treated as
  authoritative for PHI, so a deliberate *clear* is a no-op rather than a silent
  wipe. Failing the request loudly instead would also be defensible; refusing to
  destroy data in a misconfigured deployment was the direction chosen. Nothing
  surfaces the no-op to the caller either way, which is why the KEK belongs in the
  deployment's config checks.
- The server now protects the envelope, but a client should still not invite a
  non-reader to clear PHI it cannot see; `#59` (G3.3) owns the SPA side.
- The audit after-image of an update from a non-reader now records the *preserved*
  ciphertext envelope instead of `"{}"`. That is the correct record, but any tooling
  that asserted on the old (wrong) after-image would need to be updated.
