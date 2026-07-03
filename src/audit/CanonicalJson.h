// SPDX-License-Identifier: AGPL-3.0-or-later

// E5.2: Deterministic JSON serialisation (canonical form) and SHA-256 hash chain.
//
// canonical_json() implements RFC 8785 (JSON Canonicalization Scheme, JCS):
// object members are ordered by the UTF-16 code units of their keys, output is
// compact (no whitespace), strings use minimal escaping, and integers use their
// exact decimal form. The serializer builds the canonical form explicitly rather
// than relying on nlohmann's internal key ordering, so the guarantee is a
// property of this code, not of the JSON library.
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

  // Version of the canonicalization scheme. v1 == RFC 8785 (JCS). Persist this
  // alongside future audit checkpoints so a later scheme change is detectable and
  // migratable; never change canonical_json()'s behavior under a fixed version.
  inline constexpr int k_canonical_scheme_version = 1;

  // Compact, deterministic JSON per RFC 8785 (JCS): object keys ordered by UTF-16
  // code units, no whitespace, minimal escaping, exact integers.
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
