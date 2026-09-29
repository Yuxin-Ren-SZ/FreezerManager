# Handoff note — 2026-10-02, a request cannot clear a PHI key it had no control to name (#87, worker-3)

Issue **#87** is the fourth trigger of one loss class (#71, #79, #83) and the
reader-side half of it. In the three earlier cases the caller *could not see* the
PHI it destroyed. Here the caller **did** see the value: it holds `phi.read`, the
stored envelope opened, and `GetSample` returned the key. What it never had was a
**control** — the form builds its controls from the item type's *current*
definitions, so a stored PHI key whose definition was archived (or renamed) is
absent from the request, and `UpdateSample` recomputed the envelope from the
request. The archived key's ciphertext was dropped by a save that reported
success.

Chosen: **option 1**, preserve what the request cannot express. The issue's
reason for preferring it is the one that decides it — `reveal_phi` merges
decrypted values into `custom_fields_json` **without marking which keys are PHI**,
so no client can tell a key it must round-trip from an ordinary custom field. The
server is the only place that knows both the stored envelope and the current
definitions.

Branch `fix/87-custom-field-phi-preserve`, PR **#124** (`Closes #87`).

**Changed:**

- `src/storage/CustomFieldWrite.h` — `PreparedCustomFields` gains
  `std::set<std::string> current_phi_keys`, the PHI keys a *current* definition
  covers. `prepare_custom_fields` already computed that set to classify the
  request; it is now returned instead of being dropped at the end of the
  function. Keys only — never values.
- `src/server/SampleServiceImpl.cc` — `UpdateSample`'s authoritative branch. It
  started from `prepared.phi_fields_enc_json` (the request's keys, encrypted) and
  now starts from the request's values and carries over every stored key that
  `current_phi_keys` does not cover. Keys a definition *does* cover stay the
  request's to replace, blank or drop.
- `tests/integration/sample_service_integration_test.cpp` — `archive_phi_field()`
  and `defined_phi_keys()` helpers, and the two tests below.

**Decisions:**

- **The rule is stated as one sentence**: *a request can only clear a PHI key it
  could have named.* "Named" means a current definition rendered a control for it
  — not that the caller saw it, which is why `phi.read` alone cannot decide this.
- **Why not option 2 (mark PHI keys in the response).** It fixes the class
  generally but needs a wire representation and `lock:proto`, and every client
  must honour it. Option 1 needs no client change at all, which is what makes it
  the right half to land first. If a client ever *does* round-trip archived keys
  opaquely, option 2 is still the better long-term answer and this change does not
  conflict with it.
- **Why not option 3 (declare the loss).** Data loss triggered by an unrelated
  edit, on a timer, is not a product contract worth writing down when the server
  can simply not do it.
- **The envelope is still opened at most once, before anything is assigned.** The
  authoritative branch was already the one place that proves the envelope is
  openable (`caller_saw_phi` depends on the decryption succeeding, F4 on #83), and
  the merge loop only reads the map that decrypt produced. An undecryptable stored
  envelope or a KMS error therefore still fails the whole request before
  `update()`/`commit()`, leaving the ciphertext for `freezerctl key rotate` —
  covered by `UpdateSampleByPhiReaderWithUndecryptableEnvelopeFailsWithoutWriting`
  and its non-reader twin.
- **The other three writers are untouched by purpose.** `CreateSample` and both
  import paths `insert` a new row, so there is no stored envelope to preserve;
  they inherit only the additive struct field. Confirmed by reading all four call
  sites, not by assumption.
- **Non-PHI keys are out of scope here.** The plaintext column has the same
  "recomputed from the current definitions" mechanism, so a non-PHI key with an
  archived definition is still dropped on an unrelated edit. That is recoverable
  data, not ciphertext nobody can re-derive, and the issue scopes this to PHI —
  raised as a question on #87 rather than widened into this PR.

**Tests:** red first (`cd4b702`), then green (`85260d0`). The fixture change is
additive: `archive_phi_field()` soft_deletes a definition the way
`ItemTypeServiceImpl::ArchiveCustomFieldDefinition` does — a
**genuinely archived** definition, which is what the issue's second criterion asks
for, and `defined_phi_keys()` asserts the resolver no longer returns it so that
claim is checkable rather than asserted in a comment.

```
# red, source files from origin/main, tests from this branch
$ ./out/build/dev/tests/integration/freezermanager_sample_service_integration_tests \
      --gtest_filter='*Archived*'
  UpdateSampleByPhiReaderKeepsPhiKeyWhoseDefinitionWasArchived
    after.contains("age_years") -> false       # the reader's unrelated edit dropped it
  UpdateSampleByPhiReaderClearsDefinedPhiKeyAndKeepsArchivedOne
    after.contains("age_years") -> false
 2 FAILED TESTS                                                    # exit 1

# green, final tree
$ ./out/build/dev/tests/integration/freezermanager_sample_service_integration_tests \
      --gtest_filter='*Phi*:*UpdateSample*:*CustomField*:*Import*'
[  PASSED  ] 38 tests.                                              # exit 0

$ ctest --preset dev
100% tests passed out of 1593
Total Test time (real) = 134.15 sec                                 # exit 0
```

**Guards proven able to fail.** Besides the red run above (a request that
recomputes the envelope from the request), the boundary was planted on the green
tree and reverted:

- **preserve everything** — the loop's `if (!prepared.current_phi_keys.contains(key))`
  dropped, so every stored key wins over the request: **both** tests red. Test 1
  fails on `after.at("mrn") == "MRN-999"` (the request must still win for a *defined*
  key) and test 2 on `after.contains("mrn")` (a deliberate clear must still clear),
  which is exactly the half an over-eager preservation would break. Source restored
  byte-identical, re-run green.

`clang-tidy` 17.0.1, per changed TU against the unmodified `origin/main` version:
`SampleServiceImpl.cc` **23 = 23**, `src/cli/SampleCommands.cc` **19 = 19** (it
includes the changed header), and the test file is **39 → 41**, the two being
`readability-convert-member-functions-to-static` on the two new helpers. That
check also fires on **twelve** pre-existing methods in the same file — `get_sample`,
`update_sample`, `stored_phi`, `login`, … — every one of which uses fixture
members, so it is not credible on this file. Two
`readability-implicit-bool-conversion` findings on gtest message literals were
removed rather than added to, the same choice made on #62.

**Known limitations / follow-ups:**

- **A request that names a key the stored envelope holds as PHI, but which no
  current definition covers, still lands in the plaintext column** — raised as a
  question on #87. It is pre-existing (the same request did that before this
  change, *and* destroyed the ciphertext), and fixing it is a separate decision:
  refuse the key, encrypt it, or keep treating an undefined key as plaintext.
  Nothing in this PR moves the line either way; it only stops the ciphertext being
  lost alongside it.
- **Non-PHI archived keys are still dropped** on an unrelated edit (see Decisions).
- **The SPA's behaviour is unchanged and unverified here** — it builds its form
  from current definitions, so it cannot send an archived key; the client that
  *can* is one that echoes back a `GetSample` response verbatim.
