// SPDX-License-Identifier: AGPL-3.0-or-later

// Shared test-only utilities used across unit and conformance test files.
// Include this header and add `using namespace fmgr::test;` inside your anonymous
// namespace to bring the helpers into scope without qualifying every call.
#ifndef FMGR_TESTS_TEST_HELPERS_H
#define FMGR_TESTS_TEST_HELPERS_H

#include "core/timestamp.h"
#include "core/uuid.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

namespace fmgr::test {

[[nodiscard]] inline core::Uuid uuid_from_low(std::uint64_t low_bits) {
  std::array<std::uint8_t, 16> bytes{};
  for (std::size_t index = 0; index < 8; ++index) {
    bytes.at(15 - index) = static_cast<std::uint8_t>((low_bits >> (index * 8U)) & 0xffU);
  }
  return core::Uuid(bytes);
}

template <typename StrongId>
[[nodiscard]] StrongId id_from_low(std::uint64_t low_bits) {
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
