// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_STORAGE_DETAIL_AUDITCOLUMNS_H
#define FMGR_STORAGE_DETAIL_AUDITCOLUMNS_H

#include "core/audit_event.h"
#include "storage/IStorageBackend.h"

#include <string>

// Backend-neutral column-name mapping for the append-only audit_events table.
// Column names are identical across both schemas.
namespace fmgr::storage::detail {

  [[nodiscard]] inline std::string audit_event_column_name(core::AuditEvent::Field field) {
    switch (field) {
    case core::AuditEvent::Field::Id:
      return "id";
    case core::AuditEvent::Field::At:
      return "at_micros";
    case core::AuditEvent::Field::ActorUserId:
      return "actor_user_id";
    case core::AuditEvent::Field::ActorSessionId:
      return "actor_session_id";
    case core::AuditEvent::Field::LabId:
      return "lab_id";
    case core::AuditEvent::Field::Action:
      return "action";
    case core::AuditEvent::Field::EntityKind:
      return "entity_kind";
    case core::AuditEvent::Field::EntityId:
      return "entity_id";
    case core::AuditEvent::Field::BeforeJson:
      return "before_json";
    case core::AuditEvent::Field::AfterJson:
      return "after_json";
    case core::AuditEvent::Field::RequestId:
      return "request_id";
    case core::AuditEvent::Field::PrevHash:
      return "prev_hash";
    case core::AuditEvent::Field::ThisHash:
      return "this_hash";
    }
    throw ConstraintViolation("unknown AuditEvent field");
  }

} // namespace fmgr::storage::detail

#endif // FMGR_STORAGE_DETAIL_AUDITCOLUMNS_H
