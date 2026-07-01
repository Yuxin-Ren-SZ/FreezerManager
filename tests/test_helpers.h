// SPDX-License-Identifier: AGPL-3.0-or-later

// Shared test-only utilities used across unit and conformance test files.
// Include this header and add `using namespace fmgr::test;` inside your anonymous
// namespace to bring the helpers into scope without qualifying every call.
#ifndef FMGR_TESTS_TEST_HELPERS_H
#define FMGR_TESTS_TEST_HELPERS_H

#include "core/timestamp.h"
#include "core/uuid.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unistd.h>

namespace fmgr::test {

  // Build a Postgres schema name unique across parallel test *processes*.
  // gtest_discover_tests registers each TEST as its own process and ctest
  // schedules them concurrently, so a fixed per-file schema still collides when
  // two of a file's test cases run at once. Combining the PID with an in-process
  // counter guarantees no two live fixtures share a schema — this is what lets
  // us drop the old shared-`public` DROP/CREATE that raced under CI parallelism.
  [[nodiscard]] inline std::string unique_postgres_schema(std::string_view prefix) {
    static std::atomic<std::uint64_t> counter{0};
    return std::string(prefix) + "_" +
           std::to_string(static_cast<long long>(::getpid())) + "_" +
           std::to_string(counter.fetch_add(1));
  }

  // Return `base_url` with a libpq `options` parameter that pins every pooled
  // connection's search_path to `schema`. The whole backend then reads/writes
  // that schema instead of `public`, isolating the fixture from other concurrent
  // test processes. `schema` must be a bare SQL identifier (letters/digits/_).
  [[nodiscard]] inline std::string postgres_url_with_schema(const std::string& base_url,
                                                            const std::string& schema) {
    const char separator = base_url.find('?') == std::string::npos ? '?' : '&';
    return base_url + separator + "options=-csearch_path%3D" + schema;
  }

  [[nodiscard]] inline core::Uuid uuid_from_low(std::uint64_t low_bits) {
    std::array<std::uint8_t, 16> bytes{};
    for (std::size_t index = 0; index < 8; ++index) {
      bytes.at(15 - index) = static_cast<std::uint8_t>((low_bits >> (index * 8U)) & 0xffU);
    }
    return core::Uuid(bytes);
  }

  template <typename StrongId> [[nodiscard]] StrongId id_from_low(std::uint64_t low_bits) {
    return StrongId(uuid_from_low(low_bits));
  }

  [[nodiscard]] inline core::Timestamp ts(std::int64_t micros) {
    return core::Timestamp::from_unix_micros(micros);
  }

  inline void remove_sqlite_files(const std::filesystem::path& path) {
    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + "-wal");
    std::filesystem::remove(path.string() + "-shm");
  }

} // namespace fmgr::test

#endif
