// SPDX-License-Identifier: AGPL-3.0-or-later

// E5.2: Deterministic JSON serialisation (canonical form) and SHA-256 hash chain.
//
// canonical_json() produces compact, alphabetically-key-sorted JSON.
// nlohmann::json uses std::map internally for objects so keys are already sorted;
// dump(-1) produces compact output with no extra whitespace.
//
// compute_audit_hash() computes:
//   SHA-256(prev_hash_bytes || content_json_bytes)
// where both arguments are treated as UTF-8 byte sequences.
// Returns a 64-character lowercase hex string.
#ifndef FMGR_AUDIT_CANONICALJSON_H
#define FMGR_AUDIT_CANONICALJSON_H

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

namespace fmgr::audit {

  // Compact, deterministic JSON: no extra whitespace; object keys sorted.
  [[nodiscard]] std::string canonical_json(const nlohmann::json& value);

  // 64 hex '0' characters — used as prev_hash for the first audit row.
  [[nodiscard]] constexpr std::string_view zero_hash() noexcept {
    return "0000000000000000000000000000000000000000000000000000000000000000";
  }

  // hex(SHA-256(prev_hash_bytes || content_json_bytes)).
  [[nodiscard]] std::string compute_audit_hash(std::string_view prev_hash,
                                               std::string_view content_json);

} // namespace fmgr::audit

#endif // FMGR_AUDIT_CANONICALJSON_H
