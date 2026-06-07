// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_STORAGE_DETAIL_SHAREREQUESTCOLUMNS_H
#define FMGR_STORAGE_DETAIL_SHAREREQUESTCOLUMNS_H

#include "core/share_request.h"
#include "storage/IStorageBackend.h"

#include <string>

// Backend-neutral column-name mapping and pure validation for the cross-lab
// sharing entities (ShareRequest, ShareRequestApproval). Column names are
// identical across both schemas. The approval's share_request liveness check
// needs a query and stays in each backend's repository.
namespace fmgr::storage::detail {

  [[nodiscard]] inline std::string share_request_column_name(core::ShareRequest::Field field) {
    switch (field) {
    case core::ShareRequest::Field::Id:
      return "id";
    case core::ShareRequest::Field::SourceLabId:
      return "source_lab_id";
    case core::ShareRequest::Field::TargetLabId:
      return "target_lab_id";
    case core::ShareRequest::Field::RequestedBy:
      return "requested_by";
    case core::ShareRequest::Field::ScopeJson:
      return "scope_json";
    case core::ShareRequest::Field::Status:
      return "status";
    case core::ShareRequest::Field::CreatedAt:
      return "created_at_micros";
    case core::ShareRequest::Field::DecidedAt:
      return "decided_at_micros";
    }
    throw ConstraintViolation("unknown share_request field");
  }

  [[nodiscard]] inline std::string
  share_approval_column_name(core::ShareRequestApproval::Field field) {
    switch (field) {
    case core::ShareRequestApproval::Field::ShareRequestId:
      return "share_request_id";
    case core::ShareRequestApproval::Field::ApproverRole:
      return "approver_role";
    case core::ShareRequestApproval::Field::ApproverUserId:
      return "approver_user_id";
    case core::ShareRequestApproval::Field::DecidedAt:
      return "decided_at_micros";
    case core::ShareRequestApproval::Field::Note:
      return "note";
    }
    throw ConstraintViolation("unknown share_request_approval field");
  }

  inline void validate_share_request(const core::ShareRequest& request) {
    if (request.scope_json.empty()) {
      throw ConstraintViolation("share_request scope_json is required");
    }
    if (request.source_lab_id == request.target_lab_id) {
      throw ConstraintViolation("source_lab_id and target_lab_id must differ");
    }
  }

} // namespace fmgr::storage::detail

#endif // FMGR_STORAGE_DETAIL_SHAREREQUESTCOLUMNS_H
