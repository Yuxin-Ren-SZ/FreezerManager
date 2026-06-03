// SPDX-License-Identifier: AGPL-3.0-or-later

// E5.1: Hash-chained, append-only audit event.
//
// Each row stores prev_hash (the previous row's this_hash) and
//   this_hash = SHA-256(prev_hash_bytes || content_canonical_json_bytes)
// where content_canonical_json is the canonical JSON of all fields
// except prev_hash and this_hash.
// SHA-256 chosen over BLAKE2b for FIPS/compliance-auditor familiarity.
// The first row uses a zero-filled prev_hash (64 hex '0' characters).
//
// The storage layer writes these rows inside the same transaction as the
// mutating operation; the trigger layer rejects UPDATE and DELETE so the
// chain is immutable after commit.
#ifndef FMGR_CORE_AUDIT_EVENT_H
#define FMGR_CORE_AUDIT_EVENT_H

#include "core/ids.h"
#include "core/json_helpers.h"
#include "core/timestamp.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace fmgr::core {

  struct AuditEvent {
    using Id = AuditEventId;

    // Field enum used by EntityTraits / Query DSL.
    // AuditEvent has no tombstone: soft_delete() throws UnsupportedOperation.
    enum class Field : std::uint8_t {
      Id,
      At,
      ActorUserId,
      ActorSessionId,
      LabId,
      Action,
      EntityKind,
      EntityId,
      BeforeJson,
      AfterJson,
      RequestId,
      PrevHash,
      ThisHash,
    };

    AuditEventId id;
    Timestamp at;
    UserId actor_user_id;
    std::string actor_session_id;
    std::optional<LabId> lab_id; // null for global (non-lab-scoped) mutations

    // Human-readable action label, e.g. "mutation", "insert", "update",
    // "soft_delete", "auth.login", "auth.logout".
    std::string action{"mutation"};
    std::string entity_kind;              // e.g. "sample", "lab", "user"
    std::optional<std::string> entity_id; // UUID string of the affected entity

    // Canonical JSON snapshots of the entity before and after the mutation.
    // Empty object "{}" for inserts (no prior state) and bulk events.
    std::string before_json{"{}"};
    std::string after_json{"{}"};

    std::string request_id;

    // Hash chain fields — set by the storage layer, never by application code.
    std::string prev_hash; // 64-char hex; zero-filled for the first row
    std::string this_hash; // hex(SHA-256(prev_hash_bytes || content_json_bytes))

    friend bool operator==(const AuditEvent&, const AuditEvent&) = default;
  };


  inline void to_json(nlohmann::json& json, const AuditEvent& event) {
    json = nlohmann::json{
        {"id", event.id},
        {"at", event.at},
        {"actor_user_id", event.actor_user_id},
        {"actor_session_id", event.actor_session_id},
        {"lab_id", json_helpers::opt_to_json(event.lab_id)},
        {"action", event.action},
        {"entity_kind", event.entity_kind},
        {"entity_id", json_helpers::opt_to_json(event.entity_id)},
        {"before_json", event.before_json},
        {"after_json", event.after_json},
        {"request_id", event.request_id},
        {"prev_hash", event.prev_hash},
        {"this_hash", event.this_hash},
    };
  }

  inline void from_json(const nlohmann::json& json, AuditEvent& event) {
    event = AuditEvent{
        .id = json.at("id").get<AuditEventId>(),
        .at = json.at("at").get<Timestamp>(),
        .actor_user_id = json.at("actor_user_id").get<UserId>(),
        .actor_session_id = json.at("actor_session_id").get<std::string>(),
        .lab_id = json_helpers::opt_from_json<LabId>(json.at("lab_id")),
        .action = json.at("action").get<std::string>(),
        .entity_kind = json.at("entity_kind").get<std::string>(),
        .entity_id = json_helpers::opt_from_json<std::string>(json.at("entity_id")),
        .before_json = json.at("before_json").get<std::string>(),
        .after_json = json.at("after_json").get<std::string>(),
        .request_id = json.at("request_id").get<std::string>(),
        .prev_hash = json.at("prev_hash").get<std::string>(),
        .this_hash = json.at("this_hash").get<std::string>(),
    };
  }

} // namespace fmgr::core

#endif // FMGR_CORE_AUDIT_EVENT_H
