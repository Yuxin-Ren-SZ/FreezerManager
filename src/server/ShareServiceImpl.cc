// SPDX-License-Identifier: AGPL-3.0-or-later

#include "server/ShareServiceImpl.h"
#include "server/RequestId.h"

#include "core/enums.h"
#include "core/permissions.h"
#include "core/share_request.h"
#include "core/uuid.h"
#include "server/GrpcErrorTranslation.h"
#include "storage/IStorageBackend.h"
#include "storage/ShareRequestTraits.h"

#include <fmgr/v1/share.grpc.pb.h>
#include <grpcpp/grpcpp.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fmgr::server {
  namespace {

    [[nodiscard]] storage::MutationContext make_ctx(const grpc::ServerContext& ctx,
                                                    const auth::SessionContext& sctx,
                                                    std::string_view reason) {
      return storage::MutationContext{
          .actor_user_id = sctx.user_id,
          .actor_session_id = sctx.session_id.to_string(),
          .request_id = request_id_from(ctx),
          .reason = std::string(reason),
      };
    }

    [[nodiscard]] core::Timestamp now_timestamp() {
      const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();
      return core::Timestamp::from_unix_micros(static_cast<std::int64_t>(micros));
    }

    [[nodiscard]] bool is_system_admin(const auth::SessionContext& sctx) {
      return sctx.has_global(core::Permission::LabProvision);
    }

    // A page token is an opaque client-supplied integer offset; a malformed token
    // is a client error -> INVALID_ARGUMENT (see doc/CODE_REVIEW_2026-06-12.md F-4).
    [[nodiscard]] std::size_t parse_page_offset(const std::string& token) {
      try {
        std::size_t consumed = 0;
        const unsigned long long value = std::stoull(token, &consumed);
        if (consumed != token.size()) {
          throw std::invalid_argument("trailing characters");
        }
        return static_cast<std::size_t>(value);
      } catch (const std::exception&) {
        throw storage::ConstraintViolation("invalid page_token");
      }
    }

    [[nodiscard]] fmgr::v1::ShareRequestStatus to_proto_status(core::ShareRequestStatus status) {
      switch (status) {
      case core::ShareRequestStatus::Pending:
        return fmgr::v1::SHARE_REQUEST_STATUS_PENDING;
      case core::ShareRequestStatus::Approved:
        return fmgr::v1::SHARE_REQUEST_STATUS_APPROVED;
      case core::ShareRequestStatus::Rejected:
        return fmgr::v1::SHARE_REQUEST_STATUS_REJECTED;
      case core::ShareRequestStatus::Revoked:
        return fmgr::v1::SHARE_REQUEST_STATUS_REVOKED;
      }
      return fmgr::v1::SHARE_REQUEST_STATUS_UNSPECIFIED;
    }

    // UNSPECIFIED has no core counterpart; the caller must name a concrete role.
    [[nodiscard]] core::ShareApprovalRole from_proto_role(fmgr::v1::ShareApprovalRole role) {
      switch (role) {
      case fmgr::v1::SHARE_APPROVAL_ROLE_SOURCE_LAB:
        return core::ShareApprovalRole::SourceAdmin;
      case fmgr::v1::SHARE_APPROVAL_ROLE_TARGET_LAB:
        return core::ShareApprovalRole::TargetAdmin;
      case fmgr::v1::SHARE_APPROVAL_ROLE_SYSTEM_ADMIN:
        return core::ShareApprovalRole::SystemAdmin;
      case fmgr::v1::SHARE_APPROVAL_ROLE_UNSPECIFIED:
      default:
        throw storage::ConstraintViolation("approver_role must be specified");
      }
    }

    void fill_request(fmgr::v1::ShareRequest* out, const core::ShareRequest& request) {
      out->set_id(request.id.to_string());
      out->set_source_lab_id(request.source_lab_id.to_string());
      out->set_target_lab_id(request.target_lab_id.to_string());
      out->set_requested_by(request.requested_by.to_string());
      out->set_scope_json(request.scope_json);
      out->set_status(to_proto_status(request.status));
      out->mutable_created_at()->set_unix_micros(request.created_at.unix_micros());
      if (request.decided_at.has_value()) {
        out->mutable_decided_at()->set_unix_micros(request.decided_at->unix_micros());
      }
    }

    // Read gate: a principal may see a request if it holds ShareRequest for the
    // source or target lab, or is a deployment SystemAdmin.
    [[nodiscard]] bool can_read_request(const auth::SessionContext& sctx,
                                        const core::ShareRequest& request) {
      return is_system_admin(sctx) ||
             sctx.has_for_lab(request.source_lab_id, core::Permission::ShareRequest) ||
             sctx.has_for_lab(request.target_lab_id, core::Permission::ShareRequest);
    }

    // Approval gate keyed by the requested approver role. Throws PermissionDenied
    // if the caller is not entitled to sign as that role.
    void gate_approver_role(const auth::SessionContext& sctx, const core::ShareRequest& request,
                            core::ShareApprovalRole role) {
      switch (role) {
      case core::ShareApprovalRole::SourceAdmin:
        if (!sctx.has_for_lab(request.source_lab_id, core::Permission::ShareApprove)) {
          throw auth::PermissionDenied("share.approve required for the source lab");
        }
        return;
      case core::ShareApprovalRole::TargetAdmin:
        if (!sctx.has_for_lab(request.target_lab_id, core::Permission::ShareApprove)) {
          throw auth::PermissionDenied("share.approve required for the target lab");
        }
        return;
      case core::ShareApprovalRole::SystemAdmin:
        if (!is_system_admin(sctx)) {
          throw auth::PermissionDenied("system administrator required to sign as system_admin");
        }
        return;
      }
      throw storage::ConstraintViolation("unknown approver role");
    }

    // Record one approver's signature. Duplicate signatures for the same role
    // surface as ALREADY_EXISTS (the composite PK rejects them).
    void record_approval(const grpc::ServerContext& ctx, storage::ITransaction& txn,
                         const auth::SessionContext& sctx, const core::ShareRequestId& request_id,
                         core::ShareApprovalRole role, const std::optional<std::string>& note,
                         std::string_view reason) {
      const core::ShareRequestApproval approval{
          .share_request_id = request_id,
          .approver_role = role,
          .approver_user_id = sctx.user_id,
          .decided_at = now_timestamp(),
          .note = note,
      };
      txn.repo<core::ShareRequestApproval>().insert(approval, make_ctx(ctx, sctx, reason));
    }

    [[nodiscard]] std::vector<core::ShareRequestApproval>
    load_approvals(storage::ITransaction& txn, const core::ShareRequestId& request_id) {
      return txn.repo<core::ShareRequestApproval>().query(
          storage::Query<core::ShareRequestApproval>::where(
              storage::field<core::ShareRequestApproval, std::string>(
                  core::ShareRequestApproval::Field::ShareRequestId) == request_id.to_string()));
    }

    // True once all three approver roles have signed (used to flip pending ->
    // approved). Counts committed rows plus any staged in this transaction.
    [[nodiscard]] bool all_roles_signed(storage::ITransaction& txn,
                                        const core::ShareRequestId& request_id) {
      std::set<core::ShareApprovalRole> roles;
      for (const auto& approval : load_approvals(txn, request_id)) {
        roles.insert(approval.approver_role);
      }
      return roles.size() == 3;
    }

    // Separation of duties: each of the three approver roles must be signed by a
    // distinct human. Without this, one principal holding ShareApprove on both
    // labs (and system-admin) could sign all three roles and self-approve a
    // cross-lab transfer (see doc/CODE_REVIEW_2026-06-12.md F-2).
    [[nodiscard]] bool user_already_signed(storage::ITransaction& txn,
                                           const core::ShareRequestId& request_id,
                                           const core::UserId& user_id) {
      const auto approvals = load_approvals(txn, request_id);
      return std::ranges::any_of(
          approvals, [&](const auto& approval) { return approval.approver_user_id == user_id; });
    }

  } // namespace

  ShareServiceImpl::ShareServiceImpl(auth::IAuthProvider& auth, storage::IStorageBackend& backend)
      : auth_(auth), backend_(backend), middleware_(auth) {
    using P = core::Permission;
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.ShareService/ListShareRequests", P::ShareRequest);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.ShareService/GetShareRequest", P::ShareRequest);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.ShareService/CreateShareRequest", P::ShareRequest);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.ShareService/ApproveShareRequest", P::ShareApprove);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.ShareService/RejectShareRequest", P::ShareApprove);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.ShareService/RevokeShareRequest", P::ShareRequest);
  }

  grpc::Status ShareServiceImpl::CreateShareRequest(grpc::ServerContext* ctx,
                                                    const fmgr::v1::CreateShareRequestRequest* req,
                                                    fmgr::v1::CreateShareRequestResponse* resp) {
    try {
      const auto source_lab = core::LabId::parse(req->source_lab_id());
      const auto target_lab = core::LabId::parse(req->target_lab_id());
      if (source_lab == target_lab) {
        return {grpc::StatusCode::INVALID_ARGUMENT, "source_lab_id and target_lab_id must differ"};
      }
      const auto sctx =
          middleware_.authorize(extract_bearer(*ctx), core::Permission::ShareRequest, source_lab);

      // scope_json must be a JSON object describing what is shared. Reject malformed
      // or non-object payloads up front (see security-audit-2026-06-12 #6).
      const std::string scope_json =
          req->scope_json().empty() ? std::string("{}") : req->scope_json();
      if (const auto parsed = nlohmann::json::parse(scope_json, nullptr, false);
          parsed.is_discarded() || !parsed.is_object()) {
        return {grpc::StatusCode::INVALID_ARGUMENT, "scope_json must be a JSON object"};
      }

      const core::ShareRequest request{
          .id = core::ShareRequestId::parse(core::generate_uuid_v4()),
          .source_lab_id = source_lab,
          .target_lab_id = target_lab,
          .requested_by = sctx.user_id,
          .scope_json = scope_json,
          .status = core::ShareRequestStatus::Pending,
          .created_at = now_timestamp(),
      };

      auto txn = backend_.begin(storage::IsolationLevel::Serializable);
      rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);
      txn->repo<core::ShareRequest>().insert(request, make_ctx(*ctx, sctx, "create_share_request"));
      txn->commit();

      fill_request(resp->mutable_request(), request);
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

  grpc::Status ShareServiceImpl::GetShareRequest(grpc::ServerContext* ctx,
                                                 const fmgr::v1::GetShareRequestRequest* req,
                                                 fmgr::v1::GetShareRequestResponse* resp) {
    try {
      const auto request_id = core::ShareRequestId::parse(req->share_request_id());
      auto sctx = auth_.validate_token(extract_bearer(*ctx));
      if (!sctx.mfa_complete) {
        throw auth::MfaRequired("MFA required before this operation");
      }

      auto txn = backend_.begin(storage::IsolationLevel::ReadCommitted);
      rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);
      const auto request = txn->repo<core::ShareRequest>().find_by_id(request_id);
      txn->commit();

      if (!request.has_value()) {
        return {grpc::StatusCode::NOT_FOUND, "share request not found"};
      }
      if (!can_read_request(sctx, *request)) {
        throw auth::PermissionDenied("share.request required for the source or target lab");
      }
      fill_request(resp->mutable_request(), *request);
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

  grpc::Status ShareServiceImpl::ListShareRequests(grpc::ServerContext* ctx,
                                                   const fmgr::v1::ListShareRequestsRequest* req,
                                                   fmgr::v1::ListShareRequestsResponse* resp) {
    try {
      auto sctx = auth_.validate_token(extract_bearer(*ctx));
      if (!sctx.mfa_complete) {
        throw auth::MfaRequired("MFA required before this operation");
      }

      std::optional<core::LabId> source_filter;
      std::optional<core::LabId> target_filter;
      if (req->has_source_lab_id()) {
        source_filter = core::LabId::parse(req->source_lab_id());
      }
      if (req->has_target_lab_id()) {
        target_filter = core::LabId::parse(req->target_lab_id());
      }

      // A non-system-admin may only list requests for labs they hold ShareRequest
      // on, and must scope the query to such a lab — an unscoped list spans every
      // lab and is system-admin only.
      if (!is_system_admin(sctx)) {
        const bool source_ok = source_filter.has_value() &&
                               sctx.has_for_lab(*source_filter, core::Permission::ShareRequest);
        const bool target_ok = target_filter.has_value() &&
                               sctx.has_for_lab(*target_filter, core::Permission::ShareRequest);
        if (!source_ok && !target_ok) {
          throw auth::PermissionDenied(
              "share.request required for a source_lab_id or target_lab_id filter");
        }
      }

      // soft_delete() encodes "revoked" as the tombstone status, so the default
      // query already hides revoked rows. Approved/Rejected are *not* tombstoned,
      // so without an explicit status filter they would always be listed. Default
      // to pending-only; include_decided surfaces every terminal status (approved,
      // rejected, revoked). See doc/CODE_REVIEW_2026-06-12.md F-3.
      auto query = storage::Query<core::ShareRequest>::all();
      if (req->include_decided()) {
        query = query.include_tombstoned();
      } else {
        query = query.and_where(
            storage::field<core::ShareRequest, std::string>(core::ShareRequest::Field::Status) ==
            std::string(core::to_string(core::ShareRequestStatus::Pending)));
      }
      if (source_filter.has_value()) {
        query = query.and_where(storage::field<core::ShareRequest, std::string>(
                                    core::ShareRequest::Field::SourceLabId) ==
                                source_filter->to_string());
      }
      if (target_filter.has_value()) {
        query = query.and_where(storage::field<core::ShareRequest, std::string>(
                                    core::ShareRequest::Field::TargetLabId) ==
                                target_filter->to_string());
      }
      query = query.order_by(
          storage::field<core::ShareRequest, core::Timestamp>(core::ShareRequest::Field::CreatedAt),
          storage::SortDirection::Ascending);

      std::size_t offset = 0;
      if (!req->page().page_token().empty()) {
        offset = parse_page_offset(req->page().page_token());
        query = query.offset(offset);
      }
      const auto page_size = req->page().page_size();
      if (page_size > 0) {
        query = query.limit(static_cast<std::size_t>(page_size));
      }

      auto txn = backend_.begin(storage::IsolationLevel::ReadCommitted);
      rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);
      const auto requests = txn->repo<core::ShareRequest>().query(query);
      txn->commit();

      for (const auto& request : requests) {
        fill_request(resp->add_requests(), request);
      }
      if (page_size > 0 && requests.size() == static_cast<std::size_t>(page_size)) {
        resp->mutable_page()->set_next_page_token(std::to_string(offset + requests.size()));
      }
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

  grpc::Status
  ShareServiceImpl::ApproveShareRequest(grpc::ServerContext* ctx,
                                        const fmgr::v1::ApproveShareRequestRequest* req,
                                        fmgr::v1::ApproveShareRequestResponse* /*resp*/) {
    try {
      const auto request_id = core::ShareRequestId::parse(req->share_request_id());
      const auto role = from_proto_role(req->approver_role());
      auto sctx = auth_.validate_token(extract_bearer(*ctx));
      if (!sctx.mfa_complete) {
        throw auth::MfaRequired("MFA required before this operation");
      }

      auto txn = backend_.begin(storage::IsolationLevel::Serializable);
      rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);
      auto request = txn->repo<core::ShareRequest>().find_by_id(request_id);
      if (!request.has_value()) {
        return {grpc::StatusCode::NOT_FOUND, "share request not found"};
      }
      if (request->status != core::ShareRequestStatus::Pending) {
        return {grpc::StatusCode::FAILED_PRECONDITION, "share request is no longer pending"};
      }
      // Separation of duties: one human may sign at most one approver role. Checked
      // before the role gate so an already-signed principal is rejected even where
      // they would otherwise be entitled to a second role. Also subsumes a same-role
      // duplicate by the same signer (see doc/CODE_REVIEW_2026-06-12.md F-2).
      if (user_already_signed(*txn, request_id, sctx.user_id)) {
        return {grpc::StatusCode::FAILED_PRECONDITION,
                "you have already signed this share request"};
      }
      gate_approver_role(sctx, *request, role);

      const std::optional<std::string> note =
          req->has_note() ? std::optional<std::string>(req->note()) : std::nullopt;
      record_approval(*ctx, *txn, sctx, request_id, role, note, "approve_share_request");

      // Once every role has signed, the request transitions to approved.
      if (all_roles_signed(*txn, request_id)) {
        request->status = core::ShareRequestStatus::Approved;
        request->decided_at = now_timestamp();
        txn->repo<core::ShareRequest>().update(*request,
                                               make_ctx(*ctx, sctx, "approve_share_request"));
      }
      txn->commit();
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

  grpc::Status
  ShareServiceImpl::RejectShareRequest(grpc::ServerContext* ctx,
                                       const fmgr::v1::RejectShareRequestRequest* req,
                                       fmgr::v1::RejectShareRequestResponse* /*resp*/) {
    try {
      const auto request_id = core::ShareRequestId::parse(req->share_request_id());
      const auto role = from_proto_role(req->approver_role());
      auto sctx = auth_.validate_token(extract_bearer(*ctx));
      if (!sctx.mfa_complete) {
        throw auth::MfaRequired("MFA required before this operation");
      }

      auto txn = backend_.begin(storage::IsolationLevel::Serializable);
      rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);
      auto request = txn->repo<core::ShareRequest>().find_by_id(request_id);
      if (!request.has_value()) {
        return {grpc::StatusCode::NOT_FOUND, "share request not found"};
      }
      if (request->status != core::ShareRequestStatus::Pending) {
        return {grpc::StatusCode::FAILED_PRECONDITION, "share request is no longer pending"};
      }
      gate_approver_role(sctx, *request, role);

      // A single rejection is terminal — record the dissenting signature and
      // move the request to rejected immediately.
      const std::optional<std::string> note =
          req->has_note() ? std::optional<std::string>(req->note()) : std::nullopt;
      record_approval(*ctx, *txn, sctx, request_id, role, note, "reject_share_request");
      request->status = core::ShareRequestStatus::Rejected;
      request->decided_at = now_timestamp();
      txn->repo<core::ShareRequest>().update(*request,
                                             make_ctx(*ctx, sctx, "reject_share_request"));
      txn->commit();
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

  grpc::Status
  ShareServiceImpl::RevokeShareRequest(grpc::ServerContext* ctx,
                                       const fmgr::v1::RevokeShareRequestRequest* req,
                                       fmgr::v1::RevokeShareRequestResponse* /*resp*/) {
    try {
      const auto request_id = core::ShareRequestId::parse(req->share_request_id());
      auto sctx = auth_.validate_token(extract_bearer(*ctx));
      if (!sctx.mfa_complete) {
        throw auth::MfaRequired("MFA required before this operation");
      }

      auto txn = backend_.begin(storage::IsolationLevel::Serializable);
      rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);
      const auto request = txn->repo<core::ShareRequest>().find_by_id(request_id);
      if (!request.has_value()) {
        return {grpc::StatusCode::NOT_FOUND, "share request not found"};
      }
      // Only the originating (source) lab may withdraw its request.
      if (!is_system_admin(sctx) &&
          !sctx.has_for_lab(request->source_lab_id, core::Permission::ShareRequest)) {
        throw auth::PermissionDenied("share.request required for the source lab");
      }
      if (request->status != core::ShareRequestStatus::Pending &&
          request->status != core::ShareRequestStatus::Approved) {
        return {grpc::StatusCode::FAILED_PRECONDITION, "share request cannot be revoked"};
      }
      txn->repo<core::ShareRequest>().soft_delete(request_id,
                                                  make_ctx(*ctx, sctx, "revoke_share_request"));
      txn->commit();
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

} // namespace fmgr::server
