# Qt Client Test Coverage Audit

**Date:** 2026-07-01
**Audit scope:** 10 test files in `tests/unit/`
**Methodology:** 3 parallel subagents reviewed source + test files for wire-up gaps, error-path coverage, and untested code branches.

## Root Cause Pattern

All fake gRPC services return `grpc::Status::OK` unconditionally. This means every `if (!result.ok)` branch in production code is dead code from a test-coverage perspective. Additionally, tests predominantly exercise model layers directly (e.g., `BoxMapPdf::buildModel()`) rather than through the widget wire-up paths (e.g., `BoxGridWidget::printBoxMap() → BoxMapPdf::generate()`). This is the exact pattern that allowed the argument-swap bug (commit `93d3b3c`) to pass 1488 tests undetected.

---

## Gaps by Test File

### 1. `qt_box_service_client_test.cpp` — BoxServiceClient (🔴 CRITICAL)

**5/5 RPC methods have zero error-path coverage.**

| Method | Missing error test |
|--------|-------------------|
| `listFreezers` | gRPC NOT_FOUND, PERMISSION_DENIED, UNAVAILABLE |
| `listStorageContainers` | gRPC NOT_FOUND, PERMISSION_DENIED, UNAVAILABLE |
| `listBoxes` | gRPC NOT_FOUND, PERMISSION_DENIED, UNAVAILABLE |
| `getBox` | gRPC NOT_FOUND, PERMISSION_DENIED, UNAVAILABLE |
| `listBoxTypes` | gRPC NOT_FOUND, PERMISSION_DENIED, UNAVAILABLE |

All 6 existing tests are success-path only. The FakeBoxService always returns `grpc::Status::OK`.

### 2. `qt_sample_service_client_test.cpp` — SampleServiceClient (🟡)

| Gap | Method |
|-----|--------|
| `getSample` error path | gRPC NOT_FOUND, PERMISSION_DENIED |
| `exportSamplesCsv` error path | gRPC PERMISSION_DENIED, UNAVAILABLE |
| `checkoutSample` full signature | `volume_used`, `volume_unit`, `reason` optional params untested |
| `listSamples` with `item_type_id` filter | Filter mapping code branch untested |

### 3. `qt_lab_service_client_test.cpp` — LabServiceClient (🟡)

| Gap | Method |
|-----|--------|
| `getLab` error path | gRPC NOT_FOUND, PERMISSION_DENIED |

### 4. `qt_auth_service_client_test.cpp` — AuthServiceClient (🟢)

| Gap | Method |
|-----|--------|
| `logout` error path | gRPC UNAUTHENTICATED, PERMISSION_DENIED |

(Already has `login`/`submitMfa` error tests — best-tested client.)

### 5. `qt_session_manager_test.cpp` — SessionManager (🟡)

| Gap | Why |
|-----|-----|
| QTimer auto-logout (`handleTimeout()`) | Never tested with actual timer fire + event loop |
| Zero/negative TTL in `startSession` | Clamped to 0ms — edge case untested |

### 6. `qt_location_path_resolver_test.cpp` — LocationPathResolver (🔴)

| Gap | Code branch |
|-----|------------|
| Cycle guard (A→B→A) | `seen.contains(cursor)` → `partial=true` + break |
| Orphan at first cursor | Box's own container not in map |
| Empty `position_label` | Skips Position segment |
| Container with `label` set | `containerLabel()` prefers label over name |
| No freezer matches root | `listFreezers` loop finds no match |
| `listFreezers` returns `!ok` | **Potential bug:** code never checks `freezers.ok` |
| `loadContainers` mid-BFS failure | **Potential bug:** `listStorageContainers` ok unchecked |
| Deep chain (>5 levels) | Only 2-level tested |
| Multiple freezers, same root | Ambiguity handling untested |

### 7. `qt_sample_lookup_widget_test.cpp` — SampleLookupWidget (🔴)

| Gap | Code branch |
|-----|------------|
| Name-based lookup (fallback path) | barcode empty → full list → client-side name match |
| `listSamples` gRPC failure | `by_barcode.ok == false` / `lab_wide.ok == false` |
| `GetBox` gRPC failure → "Location unavailable" | Resolver error → widget error label |
| Disambiguation click → `showFound` | `matchList.itemActivated` → `showFound` + path display |
| Disambiguation click does NOT emit `sampleSelected` | Behavioral contract |
| Unplaced sample (empty box_id) | "Unplaced — not in any box." label |
| Partial path display | "(partial path — container chain incomplete)" label |
| Sample with empty barcode | "(no barcode)" label + forced fallback |
| Non-ACTIVE status badges | CHECKED_OUT, DEPLETED, DESTROYED, TOMBSTONED |
| Empty query (Enter with no text) | `performLookup()` early return |
| Initial widget state | idle page, placeholder text |
| `by_barcode.ok` false but fallback succeeds | Error-recovery path |
| Sample with empty name | Edge case |

### 8. `qt_box_grid_model_test.cpp` — BoxGridModel (🔴)

| Gap | Code branch |
|-----|------------|
| `setBox` when `getBox` returns error | `BoxGridModel.cc:24-27` |
| `setBox` when `listBoxTypes` returns error | `BoxGridModel.cc:31-33` |
| `setBox` when box_type_id not in types | `BoxGridModel.cc:43-46` |
| `rebuildOccupants` when `listSamples` returns error | Falls back to empty occupants silently |
| `gridChanged` signal verification | Never connected/received in any test |
| Accessors `labId()`, `boxId()`, `token()` | Not directly exercised |
| Empty box (zero samples) | All existing tests have at least one occupant |

### 9. `qt_box_map_pdf_test.cpp` + `qt_label_pdf_test.cpp` — PDF Tests (🔴)

#### BoxMapPdf error paths
| Gap | Code branch |
|-----|------------|
| `getBox()` gRPC failure | `buildModel()` → `m.ok=false` |
| `listBoxTypes()` failure → `setBox()` false | `buildModel()` → "failed to resolve box layout" |
| Box-type not found in types list | `setBox()` → false |
| `generate()` returns empty bytes on model failure | `if (!m.ok) return QByteArray()` |
| `listSamples()` failure in rebuildOccupants | Silent fallback to empty occupants |
| Empty box label fallback | Title falls back to `box_id` |

#### LabelPdf error paths
| Gap | Code branch |
|-----|------------|
| `getSample()` gRPC failure → skip | `if (!result.ok) continue` |
| Mixed success/failure sample list | Some resolve, some don't |
| Missing optional fields (barcode, box_id, position_label) | Location format could be malformed |

#### Wire-up paths (both PDF types — ZERO coverage)
| Gap | Wire-up |
|-----|---------|
| `BoxGridWidget::printBoxMap()` null `box_map_` | Guard clause |
| `BoxGridWidget::printBoxMap()` empty boxId | Guard clause |
| `BoxGridWidget::printBoxMap()` arg order | This EXACT path had the prior bug |
| `BoxGridWidget::printLabels()` null `labels_` | Guard clause |
| `BoxGridWidget::printLabels()` no occupied samples | Toast "No samples in this box." |
| `BoxGridWidget::printLabels()` arg correctness | Sample ID collection from `model_->cells()` |
| `BoxGridWidget::savePdf()` file write failure | QMessageBox::warning |
| `BoxGridWidget::savePdf()` user cancels | Empty path → early return |
| `LabelPrinterBridge` delegation | Token injection, correct forwarding |

#### Multi-page rendering
| Gap |
|-----|
| Box map overflows one page (undefined behavior) |
| Labels over 8 samples (>1 page — `kPerPage` boundary) |

### 10. `qt_lab_tree_model_test.cpp` — LabTreeModel (🟡)

| Gap | Code branch |
|-----|------------|
| `listFreezers` fails for one lab (others still appear) | `if (freezers.ok)` skip subtree |
| `listStorageContainers` fails at middle level | Silent skip with empty nodes |
| `listBoxes` fails for a container | `if (boxes_in.ok)` skip leaf populate |

### 11. `qt_sample_table_model_test.cpp` — SampleTableModel (🔴)

| Gap | Code branch |
|-----|------------|
| `reload()` gRPC error | `if (result.ok)` → rows empty + `has_more_` false |
| `fetchNextPage()` gRPC error | `has_more_ = false`, no row update |
| `data()` for all columns (kBarcode, kBox, kPosition, kItemType) | Only kName/kStatus/kVolume tested |
| `data()` non-DisplayRole | Should return empty QVariant |
| `data()` out-of-bounds row | Untested |
| `rowCount()`/`columnCount()` with valid parent | Should return 0 |
| `headerData()` non-DisplayRole/vertical | Untested |
| `canFetchMore()` with valid parent | Should return false |
| `fetchMore()` no-op conditions | valid parent or `!has_more_` |
| `statusToString` for ACTIVE, CHECKED_OUT, DESTROYED | Only DEPLETED/TOMBSTONED tested |
| Empty result set (filter matches nothing) | Untested |

### 12. `qt_barcode_scan_controller_test.cpp` — BarcodeScanController (🟡)

| Gap | Code branch |
|-----|------------|
| Empty/whitespace-only barcode | `if (code.isEmpty())` → "empty barcode" message |
| `listSamples` gRPC error | `if (!found.ok)` → "lookup failed: ..." |
| `scanned` signal verification | Never connected/received |
| `action()` getter | Not directly tested |
| DISCARD action verb | Only CHECKOUT/CHECKIN tested |

---

## Priority Order (fix sequence)

1. 🔴 `qt_box_service_client_test.cpp` — foundation of ALL Qt clients; 5 missing error tests
2. 🔴 `qt_sample_lookup_widget_test.cpp` — fallback path, disambiguation, error display
3. 🔴 `qt_location_path_resolver_test.cpp` — cycle guard, unchecked `listFreezers.ok` (potential bug)
4. 🔴 `qt_box_grid_model_test.cpp` — 3 setBox fail paths
5. 🔴 `qt_sample_table_model_test.cpp` — reload/fetchMore errors, all columns, edge cases
6. 🟡 `qt_session_manager_test.cpp` — QTimer auto-logout
7. 🟡 `qt_lab_tree_model_test.cpp` — mid-tree RPC failures
8. 🔴 `qt_box_map_pdf_test.cpp` + `qt_label_pdf_test.cpp` — error paths + wire-up paths
9. 🟡 `qt_barcode_scan_controller_test.cpp` — empty barcode, gRPC error
10. 🟡 `qt_sample_service_client_test.cpp` — getSample/export error, checkoutSample full sig
11. 🟡 `qt_lab_service_client_test.cpp` — getLab error
12. 🟢 `qt_auth_service_client_test.cpp` — logout error

## Fix Protocol

Each fix follows this sequence:
1. Read the source file to understand the code path being tested
2. Write the missing test(s) with appropriate fake service error injection
3. `cmake --build --preset dev` → fix build errors
4. `ctest --preset dev -j1` → all tests pass
5. clang-format on new/modified test files
6. Commit each test file fix independently
