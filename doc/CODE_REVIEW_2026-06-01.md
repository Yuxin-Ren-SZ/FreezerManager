# Code Review Report — FreezerManager v0.1

> **⚠️ Superseded by [`CODE_REVIEW_2026-06-02.md`](./CODE_REVIEW_2026-06-02.md).**
> Findings F1–F5 are resolved (F1/F4/F5 in commit `fc71cca`; F2/F3 in commit `a15bf9e`);
> F6 and F7 remain open. Kept for history. See the 2026-06-02 report for current findings,
> including 3 🔴 bugs in the new PostgreSQL backend.

**Date:** 2026-06-01
**Review scope:** Full codebase (~23,348 lines C++20 across 89 headers/sources/tests)
**Methodology:** Manual read of every source header, every `.cc` implementation, CMake build, and select test files.

---

## Overall Assessment

The foundation is **solid and well-engineered**. The domain model, storage abstraction, auth design, and audit chain are production-quality. The issues found are mechanical (helper duplication, weak validators, linear scans) rather than architectural.

---

## Findings

### F1. 🔵 `detail::optional_to_json` duplicated 7 times — ✅ RESOLVED (`fc71cca`)

> Consolidated into `src/core/json_helpers.h`; per-header copies removed.

**Files:**
- `src/core/identity.h:24–43` (`optional_to_json` / `optional_from_json`)
- `src/core/box.h:22–41` (`box_optional_to_json` / `box_optional_from_json`)
- `src/core/freezer.h:23–45` (`freezer_optional_to_json` / `freezer_optional_from_json`)
- `src/core/sample.h:25–43` (`sample_opt_to_json` / `sample_opt_from_json`)
- `src/core/session.h:104–120` (`sess_opt_to_json` / `sess_opt_from_json`)
- `src/core/share_request.h:99–115` (`sr_opt_to_json` / `sr_opt_from_json`)
- `src/core/item_type.h:23–41` (`it_optional_to_json` / `it_optional_from_json`)

**Problem:** Every header that uses `std::optional<T>` serialization defines its own copy of the same two-function template. The duplication is deliberate (each header comments "Duplicates … to avoid a cross-header dependency"), but 7 copies means any future change (e.g. supporting `std::optional<std::reference_wrapper>`, adding a `skip_null` flag) touches 7 files.

**Suggested fix:** Create a new header `src/core/json_helpers.h` with the canonical template:

```cpp
// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FMGR_CORE_JSON_HELPERS_H
#define FMGR_CORE_JSON_HELPERS_H

#include <nlohmann/json.hpp>

#include <optional>

namespace fmgr::core::json_helpers {

template <typename Value>
[[nodiscard]] inline nlohmann::json opt_to_json(const std::optional<Value>& value) {
  if (!value.has_value()) {
    return nullptr;
  }
  return nlohmann::json(value.value());
}

template <typename Value>
[[nodiscard]] inline std::optional<Value> opt_from_json(const nlohmann::json& json) {
  if (json.is_null()) {
    return std::nullopt;
  }
  return json.get<Value>();
}

} // namespace fmgr::core::json_helpers

#endif
```

Then in each affected header, replace the local detail namespace with:

```cpp
#include "core/json_helpers.h"
// Replace all box_optional_to_json() calls with json_helpers::opt_to_json()
// Replace all box_optional_from_json() calls with json_helpers::opt_from_json()
```

Delete each local copy of `detail::*optional_*`.

**Test plan:** Existing serialization tests in `tests/unit/sample_types_test.cpp`, `tests/unit/box_types_test.cpp`, `tests/unit/identity_types_test.cpp`, `tests/unit/session_types_test.cpp`, `tests/unit/share_request_types_test.cpp`, `tests/unit/item_types_test.cpp`, and `tests/unit/freezer_types_test.cpp` must continue to pass. No new test needed — this is a refactor preserving behavior.

---

### F2. 🟡 `Volume`/`Mass` cross-unit arithmetic throws at runtime — ✅ RESOLVED (`a15bf9e`)

> `to_unit()` conversion added to `Volume`/`Mass`; cross-unit add still throws by design.

**File:** `src/core/quantity.h:117–121`, `src/core/quantity.h:174–178`

**Problem:** `assert_same_unit()` throws `std::invalid_argument` when `Volume` or `Mass` operands have different units. A caller doing `volume_ml + volume_ul` gets a runtime exception rather than a converted result. The file header and inline TODOs acknowledge the gap. This is the correct *safety* posture (crash loudly rather than silently produce wrong results), but it means any RPC handler that mixes units must canonicalize before arithmetic.

**Suggested fix:** Two-phase approach:

1. **Now:** Add a `Volume::to_unit(VolumeUnit)` / `Mass::to_unit(MassUnit)` conversion method so callers have a clear path to canonicalize.
2. **Later:** Implement automatic conversion in `assert_same_unit` — canonicalize to the larger or explicitly preferred unit instead of throwing.

Phase 1 implementation:

```cpp
// In quantity.h, inside class Volume:
[[nodiscard]] constexpr Volume to_unit(VolumeUnit target) const {
  if (unit_ == target) return *this;
  // mL → µL: multiply by 1000
  if (unit_ == VolumeUnit::Milliliter && target == VolumeUnit::Microliter) {
    return {raw_value_ * 1000, target};
  }
  // µL → mL: integer divide (caller must handle truncation)
  if (unit_ == VolumeUnit::Microliter && target == VolumeUnit::Milliliter) {
    return {raw_value_ / 1000, target};
  }
  throw std::invalid_argument("unsupported volume unit conversion");
}
```

**Test plan:** Add to `tests/unit/core_value_types_test.cpp`:

```cpp
TEST(CoreValueTypes, VolumeConvertsMilliliterToMicroliter) {
  auto v = Volume::from_raw(1, VolumeUnit::Milliliter);
  auto converted = v.to_unit(VolumeUnit::Microliter);
  EXPECT_EQ(converted.raw_value(), 1000);
  EXPECT_EQ(converted.unit(), VolumeUnit::Microliter);
}

TEST(CoreValueTypes, VolumeSameUnitNoop) {
  auto v = Volume::from_raw(500, VolumeUnit::Microliter);
  auto converted = v.to_unit(VolumeUnit::Microliter);
  EXPECT_EQ(converted.raw_value(), 500);
}

TEST(CoreValueTypes, VolumeCrossUnitAddThrows) {
  auto ml = Volume::from_raw(1, VolumeUnit::Milliliter);
  auto ul = Volume::from_raw(500, VolumeUnit::Microliter);
  EXPECT_THROW(ml + ul, std::invalid_argument);
}
```

---

### F3. 🔵 Date/Datetime custom field validation accepts any string — ✅ RESOLVED (`a15bf9e`)

> ISO-8601 format validation added for Date/Datetime fields.

**File:** `src/core/custom_field_validator.h:163–170`

**Problem:**

```cpp
case FieldDataType::Date:
case FieldDataType::Datetime:
  if (!value.is_string()) {
    errors.push_back(...);
  }
  break;
```

The validator only checks that the value is a string. No format validation. A user can submit `"next Tuesday"` as a `Date` field and it passes.

**Suggested fix:** Add format validation using the `validation_json` constraint object:

```cpp
inline void validate_date_value(const std::string& key, const nlohmann::json& value,
                                const std::string& validation_json,
                                std::vector<FieldValidationError>& errors) {
  if (!value.is_string()) {
    errors.push_back({.key = key, .message = "expected string value for date"});
    return;
  }
  const auto str = value.get<std::string>();
  const auto constraints = parse_constraints(validation_json);

  // Validate ISO-8601 date format: YYYY-MM-DD
  // This is intentionally simple — no leap-second or timezone parsing.
  if (str.size() < 10 || str[4] != '-' || str[7] != '-') {
    errors.push_back({.key = key, .message = "date must be ISO-8601 format (YYYY-MM-DD)"});
    return;
  }
  if (constraints.contains("min") && constraints.at("min").is_string() &&
      str < constraints.at("min").get<std::string>()) {
    errors.push_back({.key = key, .message = "date is before minimum"});
  }
  if (constraints.contains("max") && constraints.at("max").is_string() &&
      str > constraints.at("max").get<std::string>()) {
    errors.push_back({.key = key, .message = "date is after maximum"});
  }
}

// Similarly for Datetime: YYYY-MM-DDTHH:MM:SS or YYYY-MM-DDTHH:MM:SSZ
```

Then replace the `case FieldDataType::Date:` / `case FieldDataType::Datetime:` branches in `validate_single_field()` with:

```cpp
case FieldDataType::Date:
  validate_date_value(def.key, value, def.validation_json, errors);
  break;
case FieldDataType::Datetime:
  validate_datetime_value(def.key, value, def.validation_json, errors);
  break;
```

**Test plan:** Add to `tests/unit/custom_field_validator_test.cpp`:

```cpp
TEST(CustomFieldValidator, DateFieldRejectsNonString) {
  CustomFieldDefinition def{.key = "collected_on", .data_type = FieldDataType::Date,
                             .required = false, .validation_json = "{}"};
  nlohmann::json fields{{"collected_on", 42}};
  auto errors = validate_custom_fields(std::span(&def, 1), fields);
  EXPECT_FALSE(errors.empty());
  EXPECT_EQ(errors[0].key, "collected_on");
}

TEST(CustomFieldValidator, DateFieldRejectsBadFormat) {
  CustomFieldDefinition def{.key = "collected_on", .data_type = FieldDataType::Date,
                             .required = false, .validation_json = "{}"};
  nlohmann::json fields{{"collected_on", "next Tuesday"}};
  auto errors = validate_custom_fields(std::span(&def, 1), fields);
  EXPECT_FALSE(errors.empty());
}

TEST(CustomFieldValidator, DateFieldAcceptsIso8601) {
  CustomFieldDefinition def{.key = "collected_on", .data_type = FieldDataType::Date,
                             .required = false, .validation_json = "{}"};
  nlohmann::json fields{{"collected_on", "2026-01-15"}};
  auto errors = validate_custom_fields(std::span(&def, 1), fields);
  EXPECT_TRUE(errors.empty());
}

TEST(CustomFieldValidator, DateFieldRespectsMinMax) {
  CustomFieldDefinition def{.key = "collected_on", .data_type = FieldDataType::Date,
                             .required = false, .validation_json =
                               R"({"min":"2025-01-01","max":"2025-12-31"})"};
  nlohmann::json fields{{"collected_on", "2026-06-01"}};
  auto errors = validate_custom_fields(std::span(&def, 1), fields);
  EXPECT_FALSE(errors.empty());
}
```

---

### F4. 🔵 Permission catalog lookup is O(n) linear scan — ✅ RESOLVED (`fc71cca`)

> `to_key()`/`describe()` now O(1) index; `parse_permission()` binary-searches a
> compile-time key-sorted catalog (`permissions.h:120–170`).

**File:** `src/core/permissions.h:119–144`

**Problem:** `to_key()`, `describe()`, `parse_permission()` each iterate linearly over `k_permission_catalog` (21 entries today). This is called in hot paths (auth token validation, RPC middleware). With 21 entries it's fine, but the pattern won't scale.

**Suggested fix:** Replace the linear scan with a compile-time sorted array + `std::lower_bound`. Since the catalog is `constexpr`, this can be done with zero runtime overhead beyond the binary search:

```cpp
// Sort the catalog by key at compile time.
// We maintain two catalogs: one sorted by key (for parse_permission),
// one sorted by enum value (for to_key / describe).
// Both are constexpr-sorted to avoid runtime sort overhead.

inline constexpr auto k_permission_by_key = [] {
  auto sorted = k_permission_catalog;
  // C++20 constexpr sort
  std::ranges::sort(sorted, {}, &PermissionEntry::key);
  return sorted;
}();

inline constexpr auto k_permission_by_value = [] {
  auto sorted = k_permission_catalog;
  std::ranges::sort(sorted, {}, &PermissionEntry::value);
  return sorted;
}();

[[nodiscard]] inline std::string_view to_key(Permission permission) {
  auto it = std::ranges::lower_bound(k_permission_by_value, permission, {},
                                     &PermissionEntry::value);
  if (it != k_permission_by_value.end() && it->value == permission) {
    return it->key;
  }
  throw std::invalid_argument("unknown permission");
}

[[nodiscard]] inline Permission parse_permission(std::string_view key) {
  auto it = std::ranges::lower_bound(k_permission_by_key, key, {},
                                     [](const PermissionEntry& e) { return e.key; });
  if (it != k_permission_by_key.end() && it->key == key) {
    return it->value;
  }
  throw std::invalid_argument("unknown permission key: " + std::string(key));
}
```

**Test plan:** Existing `tests/unit/permission_catalog_test.cpp` must continue to pass. No behavior change.

---

### F5. 🔵 Test helper functions duplicated across test files — ✅ RESOLVED (`fc71cca`)

> Shared helpers extracted to `tests/test_helpers.{h,cc}`.

**Affected test files:**
- `tests/unit/sqlite_sample_repository_test.cpp`
- `tests/unit/sqlite_box_repository_test.cpp`
- `tests/unit/sqlite_box_geometry_repository_test.cpp`
- `tests/unit/sqlite_identity_repository_test.cpp`
- `tests/unit/sqlite_item_type_repository_test.cpp`
- `tests/unit/sqlite_layout_repository_test.cpp`
- `tests/unit/sqlite_role_repository_test.cpp`
- `tests/unit/sqlite_share_request_repository_test.cpp`
- `tests/unit/sqlite_session_repository_test.cpp`
- `tests/unit/local_auth_provider_test.cpp`
- `tests/backend_conformance/storage_backend_conformance_test.cpp`
- `tests/backend_conformance/sqlite_backend_conformance_test.cpp`

**Problem:** The following helpers appear in nearly every SQLite test file:

- `uuid_from_low(uint64_t)` → `core::Uuid`
- `id_from_low<StrongId>(uint64_t)` → `StrongId`
- `ts(int64_t)` → `core::Timestamp`
- `mutation_context()` → `MutationContext`
- `make_lab(uint64_t)` → `core::Lab`
- `make_user(uint64_t, LabId)` → `core::User`
- `sqlite_test_path(string_view)` → `std::filesystem::path`
- `remove_sqlite_files(path)` → `void`
- SQLite backend setup/teardown boilerplate

**Suggested fix:** Create `tests/test_helpers.h`:

```cpp
// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FMGR_TESTS_TEST_HELPERS_H
#define FMGR_TESTS_TEST_HELPERS_H

#include "core/identity.h"
#include "core/ids.h"
#include "core/timestamp.h"
#include "core/uuid.h"
#include "storage/IStorageBackend.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace fmgr::test {

[[nodiscard]] inline core::Uuid uuid_from_low(std::uint64_t low_bits);

template <typename StrongId>
[[nodiscard]] inline StrongId id_from_low(std::uint64_t low_bits) {
  return StrongId(uuid_from_low(low_bits));
}

[[nodiscard]] inline core::Timestamp ts(std::int64_t micros) {
  return core::Timestamp::from_unix_micros(micros);
}

[[nodiscard]] inline storage::MutationContext mutation_context(
    std::string_view reason = "test",
    std::uint64_t user_low_bits = 999) {
  return storage::MutationContext{
    .actor_user_id = id_from_low<core::UserId>(user_low_bits),
    .actor_session_id = "test-session",
    .request_id = "test-request",
    .reason = std::string(reason),
  };
}

[[nodiscard]] inline core::Lab make_lab(std::uint64_t low_bits, std::string_view name_prefix = "Lab");

[[nodiscard]] inline core::User make_user(std::uint64_t low_bits, core::LabId lab_id,
                                          std::string_view email_prefix = "user");

[[nodiscard]] std::filesystem::path sqlite_test_path(std::string_view suffix);

void remove_sqlite_files(const std::filesystem::path& path);

} // namespace fmgr::test

#endif
```

The non-template functions go in `tests/test_helpers.cc`.

**Test plan:** All existing tests continue to pass after replacing local helpers with `fmgr::test::*` equivalents. This is a pure refactor with no behavior change.

---

### F6. ❓ Audit chain SHA-256 vs session token BLAKE2b — intentional divergence?

**Locations:**
- `src/core/session.h:56` — `token_hash` documented as "BLAKE2b hash"
- `src/core/audit_event.h:7` — audit chain documented as "SHA-256"
- `src/audit/CanonicalJson.cc:19–35` — implements SHA-256 via libsodium `crypto_hash_sha256`
- `src/auth/LocalAuthProvider.h:114` — `hash_token()` returns "BLAKE2b-256 hex"

**Question:** Is this divergence intentional, driven by different threat models?

- **Session tokens:** BLAKE2b is faster and has no known length-extension vulnerabilities. Good choice for high-frequency token verification.
- **Audit chain:** SHA-256 is widely recognized by auditors, has FIPS validation, and is more familiar in compliance contexts. Hash chaining doesn't need speed.

If intentional, suggest adding a comment in `session.h` and `audit_event.h` explaining the rationale (e.g., "SHA-256 chosen over BLAKE2b for audit chain to satisfy compliance auditor expectations"). If accidental, pick one and normalize.

No code fix required — this is a documentation question.

---

### F7. 🔵 `ITransaction` repository map uses `std::type_index` key

**File:** `src/storage/IStorageBackend.h:400–406`

**Problem:** `std::unordered_map<std::type_index, std::unique_ptr<IRepositoryBase>>` uses `std::type_index::hash_code()` which the standard guarantees is stable only within a single program execution — which is fine here. But a future reader might wonder about cross-DLL safety or serialization. Worth a one-line comment.

**Suggested fix:** Add a comment above L400:

```cpp
// type_index hash_code is stable per-process per the C++ standard — safe for
// our single-process use. If cross-DLL repository registration is ever needed,
// switch to a manual type-erased key (e.g., const char* from typeid(Entity).name()).
template <typename Entity>
void register_repository(std::unique_ptr<IRepository<Entity>> repository) {
```

No test needed.

---

## Architecture Observations

| Component | Status | Notes |
|---|---|---|
| Core domain types | ✅ Solid | 16 well-modeled entities, strong ID types, enum serialization |
| IStorageBackend + Query DSL | ✅ Clean | Type-safe predicates, sort specs, tombstone-aware queries |
| SQLite backend | ✅ Well-structured | WAL mode, busy_timeout, proper error translation, migration framework |
| Auth providers | ✅ Good | `IAuthProvider` interface, `LocalAuthProvider` covers password+TOTP+API tokens |
| Audit chain | ✅ Production-grade | Append-only + hash-chained + canonical JSON + libsodium |
| RPC/server layer | 🚧 Stubbed | CMake targets exist, no implementation |
| KMS layer | 🚧 Stubbed | No implementation |
| Qt/Web/CLI | 🚧 Stubbed | No implementation |

---

## Summary

| Severity | Count | Items | Resolution |
|---|---|---|---|
| 🔴 bug | 0 | — | — |
| 🟡 risk | 1 | F2: Volume/Mass cross-unit throws | ✅ `a15bf9e` |
| 🔵 nit | 5 | F1, F3, F4, F5, F7 | F1/F4/F5 ✅ `fc71cca`; F3 ✅ `a15bf9e`; F7 ⏳ open |
| ❓ question | 1 | F6: hash algorithm divergence | ⏳ open |

All findings are fixable with targeted, mechanical changes. No architectural redesign is needed. The project is in excellent shape for pre-alpha.

**Update 2026-06-02:** F1–F5 resolved. F6/F7 remain open. A fresh review
([`CODE_REVIEW_2026-06-02.md`](./CODE_REVIEW_2026-06-02.md)) covers the new PostgreSQL
backend and found 3 🔴 bugs there.
