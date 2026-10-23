# #170 — Custom-field columns follow the winning definition

Sample-browser custom-field cells now show a value only when the column's definition wins for that sample's item type. The column builder reuses the sample form's `resolveInheritedDefinitions` mirror of `storage::resolve_custom_field_defs`, preserving lab-wide defaults, ancestor inheritance, most-derived overrides, and the guarded lineage behavior.

Regression coverage checks unrelated root types sharing a key, inherited and lab-wide values, child overrides, unknown types, and cyclic ancestry. Column IDs and headers are unchanged; existing empty-value and PHI formatting remains in the same cell path.

Validation:

- `PATH=/Users/yuxin/Code/personal/FreezerManager/out/tools/node22/bin:$PATH npm run test -- sampleColumns.test.tsx` — 1 file passed, 13 tests passed.
- `PATH=/Users/yuxin/Code/personal/FreezerManager/out/tools/node22/bin:$PATH npm run check` — passed; 50 test files, 809 tests. ESLint emitted the existing `react-hooks/incompatible-library` warning in `src/ui/Table.tsx`.
- `ctest --preset dev` — not run in this web-only worktree; the lead is running the shared full C++ suite.
