# Resolved — 2026-06-28, Qt module clang-format + clang-tidy baseline (option 1)

The whole `src/qt/` desktop client diverged from the repo's root tooling
configs, so CI's format step and several clang-tidy checks were red on the Qt
module at baseline. Fixed via the recommended per-directory override (option 1),
mirroring the existing `tests/.clang-tidy` precedent — **zero method renames,
no namespace reflow**.

**Root cause (clang-format):** the Qt code is **Google style** (flush
namespaces, `ColumnLimit: 80`, indented case labels, return type kept on the
declaration line, 2-space trailing comments), not the root's LLVM-derived
`NamespaceIndentation: All` / `ColumnLimit: 100`. Earlier "not reproducible"
finding was a wrong base style, not a version mismatch.

**Fix shipped:**
- `src/qt/.clang-format` — `BasedOnStyle: Google`, `ColumnLimit: 80`,
  `DerivePointerAlignment: false` + `PointerAlignment: Left` (keep the
  project-wide left-aligned pointer rule), `IncludeBlocks: Preserve` (keep the
  hand-curated include groups), `ReflowComments: false` (leave authored prose
  comments). The module was then reflowed once with `clang-format-17` so the
  committed bytes match the pinned CI tool — cosmetic-only (comment padding,
  wrap shifts), no logic change.
- `src/qt/.clang-tidy` — `InheritParentConfig: true` and disables five
  house-style/framework clashes: `readability-identifier-naming` (Qt camelBack
  API + virtual overrides), `readability-redundant-access-specifiers` (moc-forced
  `private slots:` + `private:`), `bugprone-easily-swappable-parameters` (RPC
  wrappers mirror proto signatures of adjacent string ids),
  `modernize-use-nodiscard` (hits are Qt virtual overrides + trivial UI getters),
  `readability-identifier-length` (short locals — same as tests).
- **Genuine defects were FIXED in code, not suppressed:** a `qsizetype→int`
  narrowing and a `QPushButton*→bool` implicit conversion in `LoginDialog.cc`.

**Verified:** `clang-format-17 --dry-run --Werror` clean on all tracked sources;
`run-clang-tidy-17 -p out/build/dev` clean across the whole Qt module; 64/64 Qt
unit tests pass; SPDX + `git diff --check` clean.
