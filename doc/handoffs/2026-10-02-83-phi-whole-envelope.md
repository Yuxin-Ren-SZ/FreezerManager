# Handoff note — 2026-10-02, a PHI write merges per key instead of replacing the envelope (#83, worker-3)

Closes the residual boundary that #79 (`f232a94`) wrote down and left open.

`prepare_custom_fields()` recomputed the PHI envelope from the request, so a caller
**without** `phi.read` that supplied one PHI value replaced the **whole** envelope:
`{"mrn": "MRN-777"}` against a stored `{"mrn": …, "age_years": …}` left only `mrn`.
The value was stored (PHI write has never required `phi.read`, #71) and the rest was
destroyed, silently, with a normal-looking success response. Validation cannot see it:
it checks values against definitions, not which keys the caller was shown.

Whole-envelope replacement is only sound for a request that can answer for the whole
envelope. The old guard used "the caller holds `phi.read`" as the proxy for that, and
two things were wrong with the proxy:

1. **It does not imply the caller saw anything** (#82 review F4) — decryption can
   fail, and then `GetSample` returns INTERNAL and the holder was shown nothing.
2. **It is not the only source of authority the request needs** — a caller with no
   `phi.read` still supplies real values, and those values speak only for themselves.

**Changed:** `src/server/SampleServiceImpl.cc`, only. `UpdateSample`'s write decision
now reads:

- the request is **authoritative for the whole envelope** when this server could
  actually have shown the caller the stored fields — `phi.read` **and** the stored
  envelope decrypts. That branch is unchanged in effect (replace with
  `prepared.phi_fields_enc_json`), which is what keeps a holder's omitted key or
  explicit blank a deliberate clear (#79);
- otherwise the request speaks only for the keys it names with **non-blank** values:
  the stored PHI is decrypted server-side, those values are applied over it, and the
  result is re-encrypted. Keys the caller could not see survive;
- otherwise (no PHI key, or blanks only, from a caller that saw nothing) the stored
  envelope is left exactly as it was — #79's rule, unchanged.

`PreparedCustomFields` carries `phi_values` (the request's PHI keys in the clear, used
only as the merge's right-hand side) and `has_non_blank_phi_value`; `phi_keys_present`
is gone, and `has_non_empty_phi_value` is renamed (review F5 — it means non-blank).
`CreateSample` is unchanged: a new row has no stored envelope to protect.

No proto, storage, migration or REST change. No new dependency. Nothing outside
`src/server/SampleServiceImpl.cc` and the integration test.

**Decisions:**

- **Server-side per-key merge, as the lead preferred on the issue.** The invariant
  "you could not see it, therefore you cannot erase it" is then enforced where no
  client can bypass it. #85's client-side mask is still wanted (a client should not
  send what it cannot show) but is defence in depth, not the fix.
- **Authority depends on the decryption succeeding, not on the permission** (F4).
  Implemented by actually decrypting the stored envelope in that branch and
  discarding the plaintext: `crypto::decrypt` is the only honest test of "this server
  can still read it", and the discard is deliberate — the request, not the stored
  value, is what gets written.
- **Failure modes are loud, and none of them write.** An unreadable stored envelope
  and a KMS error both throw before any assignment, `update()` or `commit()`, so the
  request fails with INTERNAL and the row keeps the ciphertext `freezerctl key
  rotate` can still recover. There is no partial envelope: the new envelope string is
  computed in full and only then assigned, and `existing` is a local copy of the row
  — the sole write is `repo.update()` + `commit()`.
- **One deliberate asymmetry: a non-reader's unrelated edit on an unreadable envelope
  still succeeds and leaves the envelope alone.** Their request was never
  authoritative for stored PHI (#79), so an unreadable envelope changes nothing for
  it; failing the edit would be an availability regression with no security gain. A
  holder's edit does fail, because for them the request *is* normally authoritative
  and silently ignoring part of it would be a lie. The read path already returns
  INTERNAL on such a row, so no client can reach the edit form anyway.
- **Whitespace-only `" "` still counts as a value**, per the lead's answer on #83:
  trimming is `custom_field_validator.h`'s policy, and smuggling it into this guard
  would make the guard's meaning depend on a validator rule that can change. It is a
  decision here, not an oversight.
- **Blanks are still not writes, inside the merge too.** A non-reader that supplies
  one real value and blanks a *different* unseen key does not blank that key — the
  same destruction by a second route. The merge skips blanks with the same
  `is_blank_phi_value` helper the split loop uses.
- **`0` and `false` are values** (F2), now pinned by tests rather than by argument.

**Tests:** `tests/integration/sample_service_integration_test.cpp`. The fixture gains
two PHI definitions beside `mrn` — `age_years` (Int) and `consent_flag` (Bool) — and
two helpers: `stored_name()` (the read path cannot answer while an envelope is
unreadable) and `put_stored_phi_envelope()`, which plants an envelope wrapped under a
KEK the fixture's server does not hold, plus `orphan_phi_envelope()` to build one.

- `UpdateSampleByNonPhiReaderSupplyingOnePhiKeyKeepsTheOtherKeys` — **the decisive
  test.** Three PHI keys stored by a holder, a non-reader supplies a value for one,
  and all three are asserted after: the supplied one changed, the two it never saw
  did not. A test that checked only the supplied key passes against the old
  behaviour.
- `UpdateSampleByNonPhiReaderBlankForAnotherPhiKeyDoesNotEraseIt` — the blank route
  into the same destruction.
- `UpdateSampleByNonPhiReaderSupplyingZeroForIntPhiFieldStoresIt` /
  `…SupplyingFalseForBoolPhiFieldStoresIt` — F2, asserting the stored JSON keeps its
  `is_number_integer()`/`is_boolean()` type as well as its value (which also pins
  `FieldCipher`'s `dump()`/`parse` round-trip).
- `UpdateSampleByPhiReaderWithUndecryptableEnvelopeFailsWithoutWriting` — F4. The
  read path is INTERNAL for the row (asserted first), the unrelated edit is INTERNAL,
  the envelope is byte-identical and the name is unchanged.
  `…ByNonPhiReaderWithUndecryptableEnvelopeFailsWithoutWriting` — the same through the
  merge branch.
- `UpdateSampleByNonPhiReaderWithUndecryptableEnvelopeKeepsEnvelopeOnUnrelatedEdit` —
  the boundary above; green on arrival, it pins behaviour rather than a change.
- `UpdateSampleByNonPhiReaderWithNonEmptyPhiValueReplacesStoredPhi` was renamed to
  `…MergesIntoStoredPhi`; its name and comment asserted the behaviour this closes.

Red first (`d279c64`, the test-only commit):

```
$ ./out/build/dev/tests/integration/freezermanager_sample_service_integration_tests \
    --gtest_filter='*SupplyingOnePhiKeyKeepsTheOtherKeys*:...'
[==========] 7 tests from 1 test suite ran. (242 ms total)
[  PASSED  ] 1 test.
[  FAILED  ] 6 tests, listed below:
```

with the decisive line, verbatim:

```
sample_service_integration_test.cpp:878: Failure
Value of: after.contains("age_years")
  Actual: false
Expected: true
```

After the fix (`e0cc88e`): the same binary, unfiltered —
`[==========] 65 tests from 1 test suite ran.` / `[  PASSED  ] 65 tests.` Full
`ctest --preset dev` counts are in the PR. `clang-format --dry-run --Werror` (17.0.6)
is clean on both changed files; `tools/check-spdx-headers.sh` is unaffected (no new
file).

**Known limitations / follow-ups:**

- **A KMS error during the merge's re-encrypt is not tested end-to-end.** With
  `EnvVarKms`, `wrap_dek` cannot fail once the provider is constructed, so there is no
  reachable path to it; the only honest test needs an injection seam in
  `FreezerServerOptions`, which is a production header change beyond this issue. The
  decrypt-side failure test exercises the same "throw before commit" path, and the
  ordering argument above is what makes the property hold — but this is an argument,
  not a red-to-green test, and is reported as such.
- **clang-tidy was not run locally**: the shared clang-tidy 17.0.1 on this machine
  cannot parse this SDK's libc++ (AGENTS.md §4). CI's standalone `clang-tidy` job is
  the verdict; this handoff does not claim lint coverage.
- **A holder's request is still authoritative for the whole envelope**, so a holder
  client that drops a PHI field it was shown drops it on the server too. That is
  #79's decision (the fields were displayed, so the omission is deliberate) and is
  unchanged; it is not reachable for a non-reader any more.
- **`ImportSamples` bypasses this path entirely** — `commit_import()` inserts
  `row.sample` straight through the repository and `cli/SampleImport.cc` copies the
  CSV's `custom_fields_json` cell verbatim without resolving definitions, so a
  PHI-tagged key in that column is stored in the plaintext column and disclosed to
  any `sample.read` holder. Reported on #83 as a question for the lead to file; not
  fixed here — it is a different code path, a different fix, and this issue is the
  envelope-replacement bug.
