// SPDX-License-Identifier: AGPL-3.0-or-later

#include "audit/AuditEventContent.h"

#include "audit/CanonicalJson.h"

#include <nlohmann/json.hpp>

namespace fmgr::audit {

  std::string audit_event_content_json(const AuditEventContentFields& fields) {
    // canonical_json() orders keys, so build order here is irrelevant; the field
    // set (and their types) is what must stay stable.
    const nlohmann::json content = {
        {"action", fields.action},
        {"actor_session_id", fields.actor_session_id},
        {"actor_user_id", fields.actor_user_id},
        {"after_json", fields.after_json},
        {"at", fields.at_micros},
        {"before_json", fields.before_json},
        {"entity_id", fields.entity_id},
        {"entity_kind", fields.entity_kind},
        {"id", fields.id},
        {"lab_id", fields.lab_id.has_value() ? nlohmann::json(*fields.lab_id)
                                             : nlohmann::json(nullptr)},
        {"request_id", fields.request_id},
    };
    return canonical_json(content);
  }

  std::string audit_event_content_json(const core::AuditEvent& event) {
    return audit_event_content_json(AuditEventContentFields{
        .action = event.action,
        .actor_session_id = event.actor_session_id,
        .actor_user_id = event.actor_user_id.to_string(),
        .after_json = event.after_json,
        .at_micros = event.at.unix_micros(),
        .before_json = event.before_json,
        .entity_id = event.entity_id.value_or(""),
        .entity_kind = event.entity_kind,
        .id = event.id.to_string(),
        .lab_id = event.lab_id.has_value() ? std::optional<std::string>(event.lab_id->to_string())
                                           : std::nullopt,
        .request_id = event.request_id,
    });
  }

} // namespace fmgr::audit
