# Handoff note — 2026-10-02, a stored-PHI key is never written to the plaintext column (#126, worker-3)

Issue **#126** is the exposure direction of the key #87 protects. #87 stopped a
request from *dropping* a stored PHI key whose definition was archived; the value
was still classified against the lab's **current** custom-field definitions, so
that same key was an ordinary field for the column split. It landed in
`custom_fields_json` — the plaintext column — and `fill_sample` handed it to every
`sample.read` holder, including one without `phi.read`.

**Reachable by an ordinary client**, which is why this is a defect and not a note.
`reveal_phi` merges decrypted PHI into `custom_fields_json` **without marking
which keys are PHI**, so a client cannot tell `age_years` from an ordinary field,
and echoing a `GetSample` response back is what the edit form does. Once the
definition is archived, the client's own payload reclassifies the key as
plaintext.

**Pre-existing, stated plainly:** before #87 the same request did this *and*
destroyed the ciphertext. #87 stopped the loss; this closes the disclosure.

Branch `fix/126-stored-phi-classification`, the draft PR opened from it
(`Closes #126`). Depends on #87, merged as `9fcc8f4`; branched from `origin/main`
`19c6c17`.

**Changed:**

- `src/crypto/FieldCipher.{h,cc}` — new `envelope_field_names(envelope_json)`. It
  returns the field *names* an envelope carries, read **without the KMS and
  without opening anything**: `encrypt()` seals values and leaves keys in
  cleartext (`fields: {"<key>": {"n","c"}}`), so the set is visible in
  `phi_fields_enc_json` itself. Total by design — empty, `"{}"`, malformed or
  unrecognised envelopes yield an empty set rather than throwing, so
  classification adds no failure path the write did not already have.
- `src/storage/CustomFieldWrite.h` — `PreparedCustomFields` gains
  `phi_keys_for_classification`, and `prepare_custom_fields` gains a
  `stored_phi_keys` parameter (defaulted to empty). The **split** now runs on the
  union of the two sets; `current_phi_keys` stays definitions-only.
- `src/server/SampleServiceImpl.cc` — `UpdateSample` reads the stored envelope's
  key names (`crypto::envelope_field_names(existing->phi_fields_enc_json)`) and
  passes them in. One added call, no added branch.
- `tests/unit/field_cipher_test.cpp`, `tests/integration/sample_service_integration_test.cpp`
  — the tests below plus `stored_custom_fields()` and `set_phi_flag()` in the
  fixture.

**Decisions:**

- **The union is the rule; the envelope is evidence.** A key the stored envelope
  holds as PHI is PHI whether or not a current definition still says so, and the
  envelope does not stop being evidence when an admin archives a definition.
- **The two sets are two sets, and that is the load-bearing part of this change.**
  `current_phi_keys` (definitions only) answers *which stored keys could this
  request have named* — #87's preservation loop carries over exactly the stored
  keys that set does not contain. `phi_keys_for_classification` (the union)
  answers *which column does a request key go to*. Writing the union into
  `current_phi_keys` would make a stored-PHI-undefined key "a key the request
  could have named", the preservation loop would stop carrying it, and #87's
  silent loss would return through the path meant to close this one. Both fields
  carry that reasoning in place, because the next reader will see two similar sets
  and want to merge them.
- **Classified by key *names*, never by decrypting.** This is not only cheaper: a
  union derived from `decrypt()` would make
  `UpdateSampleByNonPhiReaderWithUndecryptableEnvelopeKeepsEnvelopeOnUnrelatedEdit`
  fail, and that test asserts something deliberate — a non-reader's unrelated edit
  of a sample with an unopenable envelope must succeed, because failing it is an
  availability regression with no security gain. The invariant the split now
  respects: **a supplied PHI write needs the envelope open; an unrelated edit does
  not.**
- **Removing `is_phi` from a definition no longer declassifies values already
  stored under that key — and that is the chosen direction, not a side effect.**
  Declassifying protected data should be an explicit, audited operation (a
  migration or a key rotation), not something that happens because someone edited
  a definition. `UpdateSampleDoesNotDeclassifyStoredPhiWhenDefinitionDropsIsPhi`
  pins it. If someone genuinely needs it, it is a migration, and it should be
  named as one.
- **One consequence is a refusal, and it is deliberate.** A request that *supplies*
  a value for a stored-PHI key with no current definition now needs the envelope
  open, because the union makes it a PHI write and a PHI write is merged into the
  stored envelope (#83). Where the envelope cannot be opened, that request now
  fails `INTERNAL` and writes nothing — where before it succeeded *and* wrote the
  value in the clear.
  `UpdateSampleOfSuppliedStoredPhiKeyWithUndecryptableEnvelopeFails` pins it, and
  the unrelated-edit boundary in the same state stays green. The same applies in
  the corners: PHI mode switched off for a lab whose envelope predates that, or no
  KMS wired, now refuses the request instead of disclosing. Refusing beats
  disclosing, and it matches the F4 rule one layer over.
- **The insert-only paths are untouched by purpose.** `CreateSample` and both
  import paths (`ImportSamples`, `freezerctl sample import`) insert a new row, so
  there is no stored envelope whose keys could be evidence; they take the
  defaulted parameter and behave exactly as before. Confirmed by reading all four
  call sites, not by assumption.
- **No new branch in either hot function.** `UpdateSample` gained a call, and
  `prepare_custom_fields` gained a set insert; neither adds a decision point, so
  neither should move CI's cognitive-complexity reading. Nothing was suppressed —
  there is no `NOLINT` in this change.

**Tests:** red first, then green — `test(server): pin that a stored PHI key stays
out of the plaintext column (#126)` then
`fix(server): classify a stored PHI key as PHI when no definition covers it (#126)`.
Named rather than hashed because the branch is rebased before ready, and a handoff
citing dead SHAs is worse than one citing none.

```
# red, source files from origin/main, tests from this branch
$ ./out/build/dev/tests/integration/freezermanager_sample_service_integration_tests \
      --gtest_filter='*PlaintextColumn*:*Declassify*'
  UpdateSampleDoesNotMoveArchivedPhiKeyIntoThePlaintextColumn
    stored_fields.contains("age_years") -> true          # the plaintext column
    as_member.custom_fields_json().find("age_years") -> 2 # and a non-reader sees it
  UpdateSampleByNonPhiReaderDoesNotPushStoredPhiKeyIntoThePlaintextColumn
    stored_phi(id).at("age_years") -> 7, expected 9
    as_member...find("age_years") -> 2
  UpdateSampleDoesNotDeclassifyStoredPhiWhenDefinitionDropsIsPhi
    parse(*plaintext).contains("mrn") -> true
    as_member...find("MRN-555") -> 8
  [  PASSED  ] 1 test.   # UpdateSampleKeepsUndefinedNonPhiKeyInThePlaintextColumn
  [  FAILED  ] 3 tests                                               # exit 1

# green, final tree
$ ctest --preset dev -R 'FieldCipher|CustomField|Phi'
100% tests passed out of 33                                        # exit 0
$ ./out/build/dev/tests/unit/freezermanager_crypto_unit_tests
[  PASSED  ] 24 tests.                                             # exit 0
$ ./out/build/dev/tests/integration/freezermanager_sample_service_integration_tests
[  PASSED  ] 83 tests.                                             # exit 0
$ ctest --preset dev
100% tests passed out of 1644
Total Test time (real) = 115.92 sec                                # exit 0
  (the 231 Postgres tests skip without FMGR_TEST_POSTGRES_URL, as they do
   locally for everyone; no GrpcTlsTest flake on this idle run)
```

Every assertion about exposure is on the **stored** `custom_fields_json` column,
read straight from storage through the new `stored_custom_fields()` helper, plus
the non-holder's `GetSample`. A response-level assertion could pass while the
column holds plaintext, which is the failure mode this issue is about.

**Guards proven able to fail.** Besides the red run above, the union itself was
planted on the green tree and reverted: removing the
`prepared.phi_keys_for_classification.insert(...)` line turns all three of the new
red-then-green tests red again while `UpdateSampleKeepsUndefinedNonPhiKeyInThePlaintextColumn`
stays green — the first three need the union, the fourth exists so a fix that
simply called every undefined key PHI cannot pass. Source restored byte-identical
and the file re-run green.

**clang-tidy — what was run, not a clean bill of health.** The local mechanism is
the documented one: the compile database drives AppleClang's `/usr/bin/c++` while
the installed clang-tidy is LLVM 17.0.1, whose libc++ uses newer builtins; the
frontend emits internal `clang-diagnostic-error`s, stops with *"too many errors
emitted"*, and **with the AST broken the semantic checks go silent while
name-lookup checks still fire**. A local run is therefore a different measurement,
not a weaker one, and this note does not claim "clang-tidy clean". What was run —
`clang-tidy -p out/build/dev --quiet <file>`, one TU at a time, **17
`clang-diagnostic-error`s per TU** from the SDK/17.0.1 mismatch as expected, with
every remaining finding on a line this change does not touch:

| TU | findings (all pre-existing) |
|---|---|
| `src/crypto/FieldCipher.cc` | `decrypt()`'s `local copy 'trimmed'` (line 113) |
| `src/storage/CustomFieldWrite.h` | `custom_field_validator.h:40` empty catch, `uuid.h:69` static-able — both in included headers |
| `src/server/SampleServiceImpl.cc` | same two included-header findings, plus `emit_new_samples` swappable params (line 377) |
| `tests/unit/field_cipher_test.cpp` | none |

**The cognitive-complexity check is CI's to report, not this machine's** — the
documented silence applies to it directly. This change adds no branch to
`UpdateSample` (one call) or to `prepare_custom_fields` (one set insert), so
nothing here is expected to move the reading; if CI disagrees, the extraction is
the answer, not a suppression.

`clang-format` **17.0.6** (the same version CI uses) on the changed files:

```
$ clang-format --dry-run --Werror src/crypto/FieldCipher.cc src/crypto/FieldCipher.h \
      src/server/SampleServiceImpl.cc src/storage/CustomFieldWrite.h \
      tests/integration/sample_service_integration_test.cpp tests/unit/field_cipher_test.cpp
(no output)                                                        # exit 0
```

**Known limitations / follow-ups:**

- **Non-PHI archived keys are still handled the #87 way** — out of scope here, and
  the plaintext column's "recomputed from the current definitions" mechanism for
  *non-PHI* keys is unchanged.
- **A migration that declassifies stored PHI does not exist.** This change makes
  that explicit rather than accidental; if a lab needs it, it is a new task, not a
  definition edit.
