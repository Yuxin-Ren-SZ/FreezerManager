# Handoff note — 2026-09-29, a blank PHI value no longer erases stored PHI (#79, worker-1)

Follow-up to #71 (`77680f6`), on the one row of the combination space its
independent review found still wrong. `prepare_custom_fields()` called a PHI key
*present* from key membership alone, so a PHI-tagged key carrying `""` or `null`
counted as a supplied PHI value and `UpdateSample` rewrote the stored envelope with
it. A caller **without** `phi.read` never receives the stored PHI (`reveal_phi()`
returns early), so `POST {"mrn": ""}` against a stored value blanked it — silently,
permanently, and with a normal-looking success response. Validation does not stop
it: for an optional string field an empty value counts as present and `null` as
absent, so either spelling passes `validate_custom_fields()`.

The #71 reasoning is unchanged and intact: writing PHI has never required
`phi.read` (`PhiWriteDoesNotRequirePhiRead`). What #79 adds is that a **blank is
not a write** — it is an erasure, and a caller that never saw the value has no
basis to intend one.

**Changed:** `src/server/SampleServiceImpl.cc` — `PreparedCustomFields` gained
`has_non_empty_phi_value`, set by the split loop in `prepare_custom_fields()` via a
new `is_blank_phi_value()` helper (`null`, or an empty string); `UpdateSample`'s
guard became `(phi_keys_present && has_non_empty_phi_value) || caller_saw_phi`,
with the distinction written out in the comment above it. Also in `UpdateSample`: a
comment on the lab check recording that `authorize(…, SampleWrite, lab_id)` uses the
**client-supplied** `lab_id` and is safe only because the `find_by_id` check
immediately after rejects the request unless `existing->lab_id == lab_id`.
`CreateSample` is unchanged: a new row has no stored envelope to protect.
`tests/integration/sample_service_integration_test.cpp` — four tests (two red
first, two regression pins). `doc/handoffs/2026-09-29-71-phi-update-overwrite.md` —
the TLS-crash paragraph said the binary "passes 5/5 when run alone"; it contains
**18** tests (`--gtest_list_tests` and `ctest -N` agree), corrected in place. The
load-artifact conclusion is unaffected.

**Decisions:**

- **A blank PHI value is refused as a write, not rejected as a request.** The
  guard is "the request is authoritative for PHI only if it supplies a value or
  the caller could have seen the fields", not "blank values are an error". A
  non-reader that sends `{"mrn": ""}` gets a successful update with the stored
  value preserved — exactly the treatment #71 gave a request that does not mention
  PHI at all. Failing loudly was the alternative; silently refusing to destroy data
  matches the merged behaviour and does not break a client that round-trips blanks.
- **"Blank" is `null` or `""`, and nothing else.** `0` and `false` are values.
  This mirrors the validator's own reading closely enough to be checkable against
  it (`null` ⇒ absent, `""` ⇒ present-but-empty) without inventing a second notion
  of emptiness.
- **A `phi.read` holder retains full authority, including over blanks.** It saw
  the stored fields, so its explicit `{"mrn": ""}` is a deliberate clear and is
  still written; so is a request carrying no PHI key at all, which clears the
  envelope. Both are pinned by tests.
- **The residual boundary is documented, not closed.** A request carrying *any*
  non-blank PHI value is authoritative for the **whole** envelope, so a non-reader
  that supplies a real value for one PHI key still replaces the others rather than
  merging into them. That is the #71 decision (non-readers may write PHI) meeting
  whole-envelope replacement — it is not specific to blanks, and closing it means a
  per-key merge against the decrypted stored envelope (a different change with its
  own failure modes), not a tweak to this guard. Written into the `UpdateSample`
  comment so the next reader does not rediscover it as a bug.
- **The lab-check comment is a comment, not a refactor.** The reviewer verified
  the invariant holds today; moving the `find_by_id` check or dropping its
  `lab_id` comparison is what would break it, and the comment names that.

**Tests:** `tests/integration/sample_service_integration_test.cpp`:

- `UpdateSampleByNonPhiReaderWithEmptyPhiValuePreservesStoredPhi` — the decisive
  test. A member without `phi.read` updates a stored `"MRN-555"` with `{"mrn":""}`;
  the raw column must not become `"{}"`, the value must still decrypt, and the
  `phi.read` holder must read it back intact.
- `UpdateSampleByNonPhiReaderWithNullPhiValuePreservesStoredPhi` — the same
  erasure by JSON `null`.
- `UpdateSampleByNonPhiReaderWithNonEmptyPhiValueReplacesStoredPhi` — the line the
  guard must not cross: a non-reader supplying a real value still replaces a stored
  envelope (the existing `…WhoSuppliesPhiStoresIt` only covered a sample with no
  PHI yet).
- `UpdateSampleByPhiReaderWithEmptyPhiValueClearsStoredPhi` — a `phi.read` holder's
  explicit blank is still honoured.

Red first (`0acab86`, the test-only commit; re-run there with the fix reverted so
the hash and the evidence match): `ctest --preset dev -R
'UpdateSampleByNonPhiReaderWithEmptyPhiValuePreservesStoredPhi' --output-on-failure`
→ `0% tests passed, 1 tests failed out of 1`, exit 8, `stored_phi(id).at("mrn")`
`""` vs `"MRN-555"`; the `null` test fails the same way with a `type must be
string, but is null`. The other two were green before the fix by construction
(they pin behaviour the old guard already had). After the fix:
`ctest --preset dev -R 'UpdateSampleBy|UpdateSampleWithout|Phi' --output-on-failure`
→ `100% tests passed out of 26` (2 Postgres tests skipped,
`FMGR_TEST_POSTGRES_URL` unset). Full `ctest --preset dev` on the final commit →
`100% tests passed out of 1450`, exit 0, 222 skipped (Postgres and the
parameterized backend suites). No rebase was needed — the branch is
`origin/main` (`77680f6`, the #71 merge) plus three commits. Repo-wide
`clang-format --dry-run --Werror` and `tools/check-spdx-headers.sh` are clean.

**Known limitations / follow-ups:**

- The whole-envelope replacement above: a non-reader's PHI write still clobbers
  PHI keys it does not mention. Needs a per-key merge or a field mask
  (`lock:proto`) — a new issue, not this one.
- While no master KEK is wired, `prepare_custom_fields()` throws on *any*
  PHI-tagged key, blanks included, so a non-reader sending `{"mrn": ""}` against a
  KMS-less server gets an `Internal` error rather than the silent preservation the
  KMS-wired path gives. Pre-existing (it is the same branch that errors for a real
  value), loud rather than destructive, and unchanged by this fix.
- Nothing here changes the SPA side; `#66`/G3.3 still own not inviting a
  non-reader to submit fields it cannot see.
