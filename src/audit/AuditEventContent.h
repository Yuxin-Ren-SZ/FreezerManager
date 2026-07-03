// SPDX-License-Identifier: AGPL-3.0-or-later

// Single source of truth for the hashed representation of an audit row. The
// storage backends compute this_hash = SHA-256(prev_hash || content_json) at
// insert time, and AuditChainVerifier recomputes it when verifying. Both must
// hash byte-identical content, so the exact field set and canonicalization live
// here rather than being duplicated at each call site (review — audit
// canonicalization: content object was previously built in three places).
#ifndef FMGR_AUDIT_AUDITEVENTCONTENT_H
#define FMGR_AUDIT_AUDITEVENTCONTENT_H

#include "core/audit_event.h"

#include <cstdint>
#include <optional>
#include <string>

namespace fmgr::audit {

  // The raw fields the backends have on hand at insert time. `entity_id` is the
  // bound value (empty string when absent, never null). `lab_id` is nullopt when
  // the row has no lab, which canonicalizes to JSON null.
  struct AuditEventContentFields {
    std::string action;
    std::string actor_session_id;
    std::string actor_user_id;
    std::string after_json;
    std::int64_t at_micros{0};
    std::string before_json;
    std::string entity_id;
    std::string entity_kind;
    std::string id;
    std::optional<std::string> lab_id;
    std::string request_id;
  };

  // Canonical JSON (RFC 8785) of exactly the 11-key content object, excluding
  // prev_hash/this_hash. The one place the hashed field set is defined.
  [[nodiscard]] std::string audit_event_content_json(const AuditEventContentFields& fields);

  // Convenience overload for a fully-formed AuditEvent (used by the verifier).
  [[nodiscard]] std::string audit_event_content_json(const core::AuditEvent& event);

} // namespace fmgr::audit

#endif // FMGR_AUDIT_AUDITEVENTCONTENT_H
