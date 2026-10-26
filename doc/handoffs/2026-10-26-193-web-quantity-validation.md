# #193 web quantity validation handoff

## Change

- Sample create/edit form quantity fields now advertise whole nonnegative values (`min=0`, `step=1`, localized hint), and client validation rejects fractions, negatives, non-finite values and values at or above 2^63 before a request is sent.
- The fake API applies the same rule to sample create/update and checkout `volume_used`; it validates before changing stored rows or appending checkout events. Update remains full replacement: omitted optional quantity fields clear the stored values.
- Tests cover accepted values unchanged, the largest JavaScript-representable integer below 2^63, 0.5, negative values, 1e19, 2^63, NaN and +inf, atomic rejection, update clearing, and form-side no-request behavior.

## Validation

- `NODE_ENV=development npm ci --cache "$PWD/../../out/npm-cache"` — installed 469 packages from the committed lockfile. npm reported 6 audit advisories (2 moderate, 4 high); no manifest or lockfile changes were made.
- `npm run test -- --run src/test/fakeApi.test.ts src/features/sample-detail/SampleForm.test.tsx` — 2 files, 77 tests passed.
- `npm run check` — passed: route check, ESLint (0 errors; one existing Table compiler warning), TypeScript, Prettier, 52 test files / 840 tests, production build. Initial JS bundle: 203.6 KiB gzip (250 KiB budget).
- `tools/check-spdx-headers.sh` and `git diff --check` — recorded after the final edit.
- C++ build and CTest were not run; the lead waived them for this web-only module.

## Wire-value limits

The fake and form use JavaScript `Number` values. Protobuf JSON supports NaN and infinities as special string values, and the API regression tests exercise NaN and +inf through the generated client. JSON cannot preserve negative zero as a distinct value; it is nonnegative for this rule. Integer values above 2^53 are only represented at JavaScript's spacing (for example, the largest representable Number below 2^63 is 2^63 - 1024); that boundary is tested unchanged. The fake `sample/import` route currently has no dedicated resolver, so this web change does not claim import-path fake parity.
