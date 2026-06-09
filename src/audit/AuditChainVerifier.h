// SPDX-License-Identifier: AGPL-3.0-or-later

// Verifies the integrity of the append-only audit hash chain (PRD §7.3).
//
// The storage backends write each audit row with
//   this_hash = SHA-256(prev_hash_bytes || canonical_content_json_bytes)
// chaining prev_hash to the previous row's this_hash (first row uses
// zero_hash()). This module recomputes that chain over an ordered run of
// AuditEvent rows and reports the first divergence, detecting either a broken
// link (prev_hash no longer matches the predecessor) or a tampered row
// (recomputed this_hash no longer matches the stored one).
//
// Pure and DB-free: the same logic verifies rows read from any IStorageBackend.
#ifndef FMGR_AUDIT_AUDITCHAINVERIFIER_H
#define FMGR_AUDIT_AUDITCHAINVERIFIER_H

#include "audit/CanonicalJson.h"
#include "core/audit_event.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fmgr::audit {

  // Canonical JSON of exactly the fields the backends hash into this_hash — the
  // 11-key content object, excluding prev_hash/this_hash. Single source of truth
  // for the hashed representation of an audit row.
  [[nodiscard]] std::string audit_event_content_json(const core::AuditEvent& event);

  enum class AuditChainStatus : std::uint8_t {
    Ok,
    BrokenLink,   // prev_hash does not match the predecessor's this_hash
    HashMismatch, // recomputed this_hash does not match the stored one (tamper)
  };

  struct AuditChainError {
    std::size_t index;       // position in the ordered run (0-based)
    core::AuditEventId id;   // id of the offending row
    AuditChainStatus status; // BrokenLink or HashMismatch
    std::string detail;      // human-readable explanation
  };

  struct AuditChainReport {
    bool ok{true};
    std::size_t verified_count{0};
    std::optional<AuditChainError> first_error;
  };

  // Walk `ordered` (ascending insertion order: at_micros ASC, id ASC) and verify
  // the chain. The first row's prev_hash must equal `expected_first_prev`
  // (zero_hash() for a full chain from the beginning). Returns the first
  // divergence, or ok with verified_count == ordered.size().
  [[nodiscard]] AuditChainReport
  verify_audit_chain(const std::vector<core::AuditEvent>& events,
                     std::string_view expected_first_prev = zero_hash());

} // namespace fmgr::audit

#endif // FMGR_AUDIT_AUDITCHAINVERIFIER_H
