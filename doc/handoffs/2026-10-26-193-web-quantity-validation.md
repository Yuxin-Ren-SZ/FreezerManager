# #193 web quantity validation handoff

## Change

- Sample create/edit form quantity fields advertise whole nonnegative values (`min=0`, `step=1`, localized hint). Client validation rejects fractions, negatives, non-finite values, values at or above 2^63, and decimal text that would round to a different whole `Number`, before a request is sent.
- Raw integer, decimal-point and scientific-exponent spellings are normalized to a coefficient and decimal scale and compared with `BigInt(Number(raw))`, the exact integer being submitted. No exponentiation or dependency is needed. Exactly represented whole doubles above 2^53 remain accepted; existing whole quantities are displayed using their exact integer digits so an untouched large edit remains valid.
- The fake API applies the same rule to sample create/update and checkout `volume_used`; it validates before changing stored rows or appending checkout events. Update remains full replacement: omitted optional quantity fields clear the stored values.
- Tests cover accepted values unchanged, the largest JavaScript-representable integer below 2^63, 0.5, negative values, 1e19, 2^63, NaN and +inf, atomic rejection, update clearing, and form-side no-request behavior. Revision cases include `9007199254740993`, `9007199254740992.5`, equivalent exponent forms, and underflow to zero; accepted decimal/exponent and large integer forms; and edit-side mass refusal, exact large stored-value display/save, and optional clearing.

## Validation

- `NODE_ENV=development npm ci --cache "$PWD/../../out/npm-cache"` — installed 469 packages from the committed lockfile. npm reported 6 audit advisories (2 moderate, 4 high); no manifest or lockfile changes were made.
- Revision TDD: `npm run test -- --run src/features/sample-detail/SampleForm.test.tsx` — 9 failed / 29 passed before the helper and display changes, reproducing rounded create/edit acceptance and inexact stored-value display.
- `npm run test -- --run src/test/fakeApi.test.ts src/features/sample-detail/SampleForm.test.tsx` — 2 files, 96 tests passed after the revision. The fake boundary tests remain unchanged.
- `npm run check` — passed: route check, ESLint (0 errors; one existing Table compiler warning), TypeScript, Prettier, 52 test files / 859 tests, production build. Initial JS bundle: 203.6 KiB gzip (250 KiB budget). Node v22.23.3 from the main checkout's `out/tools/node22/bin` was used.
- `tools/check-spdx-headers.sh` and `git diff --check` — passed.
- C++ build and CTest were not run; the lead waived them for this web-only module.

## Wire-value limits

The fake and form submit JavaScript `Number` values. Protobuf JSON supports NaN and infinities as special string values, and the API regression tests exercise NaN and +inf through the generated client. JSON cannot preserve negative zero as a distinct value; it is nonnegative for this rule. Integer values above 2^53 are only represented at JavaScript's spacing (for example, the largest representable Number below 2^63 is 2^63 - 1024); that boundary is tested unchanged. The fake retains this wire-value contract; the form additionally requires the entered decimal text to denote that exact integer. JSON may serialize an accepted double with a shorter decimal spelling, which parses back to the same double. The fake `sample/import` route currently has no dedicated resolver, so this web change does not claim import-path fake parity.
