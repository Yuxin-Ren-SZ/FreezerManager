# Handoff note — 2026-10-02, the sample form stops submitting PHI it never showed (#85, worker-3)

`SampleForm`'s `wireValue()` returns `raw === true` for `BOOL` and `defaultRaw()`
returns `false` (`SampleForm.tsx:79-110` before this change), so **a Bool field was
always in the payload** — `false !== undefined`, where String, Int, Float and Date
omit an empty value. A caller without `phi.read` gets no PHI key in the response at
all (`reveal_phi()` in `SampleServiceImpl.cc`), so on an edit the form invented
`false` for fields whose values it had never been shown. `false` is a *value*: it
made `prepared.phi_keys_present` true, and #71's gate
(`phi_keys_present || caller_saw_phi`) then wrote the recomputed envelope over the
stored one. Every other PHI key on the sample was destroyed, permanently, with a
normal successful save as the only feedback. This is the client half of #83; both
are wanted, and either alone leaves a path open. Branch `fix/85-form-phi-masking`,
PR **#86**.

This was the third variant of the #71/#79/#83 loss class and the first one the SPA
could actually trigger. #71's handoff said the SPA side belonged to #59 (G3.3);
#59 is where the form was written, and this is the follow-up it could not see.

**Changed:**

- `src/web/src/features/sample-detail/SampleForm.tsx` — one derived flag,
  `phiWithheld = isEdit && !canReadPhi`; `wireValues` skips a PHI definition when
  it is set; the PHI controls render `disabled`; `CustomFieldInput` takes
  `disabled` and passes it to `Checkbox`/`Select`/`TextField`; the file docstring's
  third contract now states the rule instead of the claim that was false for Bool.
- `src/web/src/features/sample-detail/SampleForm.test.tsx` — a new
  `describe('SampleForm: PHI the caller cannot read')` block, three tests, plus the
  capture helper they assert on.
- `src/web/locales/en/sample-detail.json` — `form.phiWriteWarning` rewritten.

**Decisions:**

- **The gate is permission-and-edit, not field type.** On an edit, a caller without
  `phi.read` has been shown *none* of the PHI values, so no PHI field can be
  submitted — fixing only Bool would have left the same class of bug to the next
  field type whose empty control is a value. The withheld fields are now absent
  structurally rather than by accident of `''` omitting a key.
- **A create is deliberately unchanged.** Nothing is stored yet, so nothing is
  withheld, and writing PHI has never required `phi.read`
  (`PhiWriteDoesNotRequirePhiRead`, pinned by #71's integration test). The client
  mirrors the server here rather than inventing a rule the server does not have.
  The consequence to be aware of: the same PHI control is editable on create and
  disabled on edit for that caller. Each state is right for its own situation.
- **The withheld controls are `disabled`.** The payload fix alone would let someone
  type a donor name into a control whose value is then dropped on submit — the
  "Changes saved." message would be a lie about that field. Disabling is what makes
  the corrected warning true in the UI and not only on the wire. A disabled control
  is still in `rawValues` as `defaultRaw(cfd)` (`false` for Bool), so the payload
  omission is done by `wireValues`, not by the `disabled` attribute — the decisive
  test fails on the unfixed tree with or without it.
- **`form.phiWriteWarning` corrected.** It said "Saving this form clears them",
  which has been untrue since #71 for String fields (an empty value is omitted and
  the stored value is preserved) and is untrue for Bool now that nothing is sent.
  The new text says what the form does — the stored values are hidden, not
  submitted, left as they are, and anything typed into them is not saved. A test
  asserts the copy no longer says "clear", so the specific inaccuracy cannot come
  back.
- **"Explicitly clearing a visible field" has no wire representation of its own.**
  There is no field mask in the proto, so the payload shapes are: visible String
  cleared → key absent; visible Bool unchecked → `false`; withheld field → no PHI
  key at all. The first two are the same shapes a `phi.read` holder sent before, and
  the third is what the server reads as "this caller cannot be authoritative for
  PHI" (#71's `caller_saw_phi`). Telling a deliberate clear from a withheld field
  therefore depends on the caller's permission, not on the payload — a real
  mask/unset needs a proto change (`lock:proto`), which is why it is recorded here
  rather than done.
- **No new dependency, no shared-file edit.** `package.json` and
  `package-lock.json` are untouched, and so is `src/test/fakeApi.ts`: the fake
  seeds a Text PHI field but no Bool PHI one, so the test file pushes its own
  `CustomFieldDefinition` into `DemoLab.customFieldDefs`, which is the fixture's
  supported way to vary the seed.

**Tests:** `src/web/src/features/sample-detail/SampleForm.test.tsx`

- `leaves a withheld Bool PHI field out of the payload rather than sending false` —
  the decisive test. It mounts `SampleDetailScreen` as a caller with
  `['sample.read', 'sample.write']` — `sample.read` is what the definitions route
  is gated on since #69 (not `custom_field.define`), so the form renders the PHI
  control; `phi.read` is absent, so the response has no PHI value. The test asserts
  the control **is** in the document, then that the **whole `custom_fields_json` of
  the request body** omits the key — captured off the wire by an MSW handler
  registered after mount, so the assertion is about what a *successful* save sends
  rather than about what the fake stored. Red first:

  ```
  × leaves a withheld Bool PHI field out of the payload rather than sending false
      {
        "aliquot_count": 3, "collection_date": "2026-01-05", "concentration": 12.5,
    +   "donor_screening_flag": false,
        "is_hemolyzed": true, "notes": "ok",
      }
  × offers no PHI control it could not fill with the stored value   (not disabled)
    Tests  2 failed | 19 passed (21)
  ```

- `offers no PHI control it could not fill with the stored value` — the control is
  disabled, the notice renders, and the copy no longer contains "clear".
- `sends an explicit false when a caller that can read PHI unchecks the box` — a
  `phi.read` holder's deliberate unset still reaches the wire as `false`, which is
  distinguishable from the withheld case above (absent). This one passed before the
  change too: it is the no-regression pin.

Green after the fix — exact commands and results, on the final commit (rebased onto
`d0a4e1c`, which is #76's `sample.read` gate on the reference data; the counts moved
from `33 files / 516 tests` to `34 / 519` because #76 added
`useSampleReferenceData.test.tsx`, not because of anything here):

```sh
$ npm run test -- src/features/sample-detail/SampleForm.test.tsx
 Test Files  1 passed (1)
      Tests  21 passed (21)

$ env -u NODE_ENV npm run check
check-routes: ok — 70 unary routes and 2 SSE routes agree between RestGateway.cc, routes.ts and sse.ts
✖ 1 problem (0 errors, 1 warning)   # pre-existing, src/ui/Table.tsx react-hooks/incompatible-library
 Test Files  34 passed (34)
      Tests  519 passed (519)
✓ built in 290ms
exit=0

$ NODE_ENV=production npm run check
 Test Files  34 passed (34)
      Tests  519 passed (519)
✓ built in 183ms
exit=0
```

(The bundle guard reports `initial JS 158.7 KiB gzipped, budget 250 KiB` on
`main` plus this change — the fix adds no code to the entry chunk.)

**The rebase was semantic, not mechanical, and was verified as such.** #76 changed
*which permission* makes the definitions readable (`sample.read`, not
`custom_field.define`), so a test of the shape "the withheld key is absent" could
have passed for the wrong reason afterwards — a caller the form refuses to render
custom fields for would send no PHI key either. Two guards:

1. The decisive test asserts the PHI control **is rendered** before it asserts the
   payload, so "absent from the request" cannot be confused with "never on screen".
   The whole-blob equality does the same job from the other side: five non-PHI keys
   are still there, so the form was not empty.
2. A mutation check on the rebased tree: deleting the single
   `if (phiWithheld && cfd.isPhi) continue;` line makes the test fail again with
   exactly `+ "donor_screening_flag": false` (`1 failed | 20 passed`), which is only
   reachable if the control rendered and went through `wireValues`. The line was
   restored and the tree is clean.

No C++ file is touched, so `ctest` is not this change's gate; CI's `web` job runs
the same `npm ci && npm run check`.

**Known limitations / follow-ups:**

- **A `phi.read` holder's save still recomputes the envelope from the form's
  definitions**, so a stored PHI key with no current definition — an archived one,
  or one written by a client with a different definition set — is still dropped by
  an unrelated edit. The client cannot do better: the response merges PHI into
  `custom_fields_json` without marking which keys are PHI, so an unrecognised key
  cannot be recognised as one to carry through. That belongs to #83.
- **A server with `phi.read` granted but no master KEK wired is not distinguishable
  client-side.** `reveal_phi()` returns early for everyone there, so a holder's
  Bool PHI fields are still submitted as `false`; `prepare_custom_fields()` then
  throws (`server is not configured with a master key`) before any write, so the
  outcome is a loud failure on an unrelated edit, not silent loss. Pinning that on
  the client would mean sending a probe the server has no way to answer.
- **A required PHI field on the item type still blocks a non-reader's edit**, with
  a "required" error on a control they cannot fill — as it already did for String
  fields before this change (the key was omitted, so the client mirror failed it)
  and as the server would anyway. Nothing new, but it is the one place where the
  withheld-field rule and the required-field rule collide, and `disabled` makes it
  visible rather than typeable-around.
