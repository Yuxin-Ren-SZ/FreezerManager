// SPDX-License-Identifier: AGPL-3.0-or-later

// D8: Cross-lab share-request workflow.
// ShareRequest state machine: pending -> approved | rejected | revoked.
// Approval requires all three roles (source_admin, target_admin, system_admin).
// ShareRequestApproval is append-only; rows are never updated or soft-deleted.
// The repository does NOT enforce state-machine transitions — that is the RPC layer's job.
#ifndef FMGR_CORE_SHARE_REQUEST_H
#define FMGR_CORE_SHARE_REQUEST_H

#include "core/enums.h"
#include "core/ids.h"
#include "core/json_helpers.h"
#include "core/timestamp.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace fmgr::core {

  // ---- ShareRequestApprovalId ----

  // Composite primary key for the share_request_approvals table.
  // At most one approval row per (share_request_id, approver_role).
  struct ShareRequestApprovalId {
    ShareRequestId share_request_id;
    ShareApprovalRole approver_role;

    friend constexpr auto operator<=>(const ShareRequestApprovalId&,
                                      const ShareRequestApprovalId&) = default;
    friend constexpr bool operator==(const ShareRequestApprovalId&,
                                     const ShareRequestApprovalId&) = default;
  };

  // ---- ShareRequestApproval ----

  // Immutable audit record of a single approver's decision on a share request.
  // Once inserted these rows are never updated or soft-deleted (append-only audit).
  struct ShareRequestApproval {
    using Id = ShareRequestApprovalId;

    enum class Field : std::uint8_t {
      ShareRequestId,
      ApproverRole,
      ApproverUserId,
      DecidedAt,
      Note,
    };

    ShareRequestId share_request_id;
    ShareApprovalRole approver_role;
    UserId approver_user_id;
    Timestamp decided_at;
    std::optional<std::string> note;

    [[nodiscard]] ShareRequestApprovalId id() const {
      return ShareRequestApprovalId{share_request_id, approver_role};
    }

    friend bool operator==(const ShareRequestApproval&, const ShareRequestApproval&) = default;
  };

  // ---- ShareRequest ----

  // Cross-lab sharing request.
  // scope_json describes which samples/projects are shared (validated at RPC layer).
  // soft_delete() sets status = revoked.
  // Default query filter: status = 'pending'; include_tombstoned() removes the filter.
  struct ShareRequest {
    using Id = ShareRequestId;

    enum class Field : std::uint8_t {
      Id,
      SourceLabId,
      TargetLabId,
      RequestedBy,
      ScopeJson,
      Status,
      CreatedAt,
      DecidedAt,
    };

    ShareRequestId id;
    LabId source_lab_id;
    LabId target_lab_id;
    UserId requested_by;
    std::string scope_json{"{}"}; // validated against a schema at RPC layer
    ShareRequestStatus status{ShareRequestStatus::Pending};
    Timestamp created_at;
    std::optional<Timestamp> decided_at;

    friend bool operator==(const ShareRequest&, const ShareRequest&) = default;
  };

  // ---- JSON serialization ----


  inline void to_json(nlohmann::json& json, const ShareRequestApprovalId& id) {
    json = nlohmann::json{
        {"share_request_id", id.share_request_id},
        {"approver_role", id.approver_role},
    };
  }

  inline void from_json(const nlohmann::json& json, ShareRequestApprovalId& id) {
    id = ShareRequestApprovalId{
        .share_request_id = json.at("share_request_id").get<ShareRequestId>(),
        .approver_role = json.at("approver_role").get<ShareApprovalRole>(),
    };
  }

  inline void to_json(nlohmann::json& json, const ShareRequestApproval& approval) {
    json = nlohmann::json{
        {"share_request_id", approval.share_request_id}, {"approver_role", approval.approver_role},
        {"approver_user_id", approval.approver_user_id}, {"decided_at", approval.decided_at},
        {"note", json_helpers::opt_to_json(approval.note)},
    };
  }

  inline void from_json(const nlohmann::json& json, ShareRequestApproval& approval) {
    approval = ShareRequestApproval{
        .share_request_id = json.at("share_request_id").get<ShareRequestId>(),
        .approver_role = json.at("approver_role").get<ShareApprovalRole>(),
        .approver_user_id = json.at("approver_user_id").get<UserId>(),
        .decided_at = json.at("decided_at").get<Timestamp>(),
        .note = json_helpers::opt_from_json<std::string>(json.at("note")),
    };
  }

  inline void to_json(nlohmann::json& json, const ShareRequest& request) {
    json = nlohmann::json{
        {"id", request.id},
        {"source_lab_id", request.source_lab_id},
        {"target_lab_id", request.target_lab_id},
        {"requested_by", request.requested_by},
        {"scope_json", request.scope_json},
        {"status", request.status},
        {"created_at", request.created_at},
        {"decided_at", json_helpers::opt_to_json(request.decided_at)},
    };
  }

  inline void from_json(const nlohmann::json& json, ShareRequest& request) {
    request = ShareRequest{
        .id = json.at("id").get<ShareRequestId>(),
        .source_lab_id = json.at("source_lab_id").get<LabId>(),
        .target_lab_id = json.at("target_lab_id").get<LabId>(),
        .requested_by = json.at("requested_by").get<UserId>(),
        .scope_json = json.at("scope_json").get<std::string>(),
        .status = json.at("status").get<ShareRequestStatus>(),
        .created_at = json.at("created_at").get<Timestamp>(),
        .decided_at = json_helpers::opt_from_json<Timestamp>(json.at("decided_at")),
    };
  }

} // namespace fmgr::core

#endif // FMGR_CORE_SHARE_REQUEST_H
