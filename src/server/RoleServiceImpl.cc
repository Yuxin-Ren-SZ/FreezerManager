// SPDX-License-Identifier: AGPL-3.0-or-later

#include "server/RoleServiceImpl.h"

#include "core/enums.h"
#include "core/permissions.h"
#include "core/role.h"
#include "server/GrpcErrorTranslation.h"
#include "storage/IStorageBackend.h"
#include "storage/RoleTraits.h"

#include <fmgr/v1/role.grpc.pb.h>
#include <grpcpp/grpcpp.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <optional>
#include <random>
#include <string>
#include <string_view>

namespace fmgr::server {
  namespace {

    [[nodiscard]] storage::MutationContext make_ctx(const auth::SessionContext& sctx,
                                                    std::string_view reason) {
      return storage::MutationContext{
          .actor_user_id = sctx.user_id,
          .actor_session_id = sctx.session_id.to_string(),
          .request_id = "",
          .reason = std::string(reason),
      };
    }

    [[nodiscard]] core::Timestamp now_timestamp() {
      const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();
      return core::Timestamp::from_unix_micros(static_cast<std::int64_t>(micros));
    }

    // RFC 4122 version-4 UUID from a non-deterministic entropy source. IDs must be
    // unguessable but need not be cryptographically secret (see BoxServiceImpl).
    [[nodiscard]] std::string generate_uuid_v4() {
      std::random_device rng;
      std::array<std::uint8_t, 16> bytes{};
      for (std::size_t i = 0; i < bytes.size(); i += 4) {
        const std::uint32_t word = rng();
        bytes[i] = static_cast<std::uint8_t>(word & 0xFFU);
        bytes[i + 1] = static_cast<std::uint8_t>((word >> 8U) & 0xFFU);
        bytes[i + 2] = static_cast<std::uint8_t>((word >> 16U) & 0xFFU);
        bytes[i + 3] = static_cast<std::uint8_t>((word >> 24U) & 0xFFU);
      }
      bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0FU) | 0x40U); // version 4
      bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3FU) | 0x80U); // variant
      std::array<char, 37> buf{};
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
      std::snprintf(buf.data(), buf.size(),
                    "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                    bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
                    bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14],
                    bytes[15]);
      return {buf.data()};
    }

    // ---- RoleKind mapping ----
    //
    // The core enum (SystemAdmin/LabAdmin/Member/ReadOnly/ApiClient) and the wire
    // enum (UNSPECIFIED + the same five) share the five named members; UNSPECIFIED
    // has no core counterpart and maps to Member, the least-privileged default
    // appropriate for a newly created custom role.

    [[nodiscard]] fmgr::v1::RoleKind to_proto_role_kind(core::RoleKind kind) {
      switch (kind) {
      case core::RoleKind::SystemAdmin:
        return fmgr::v1::ROLE_KIND_SYSTEM_ADMIN;
      case core::RoleKind::LabAdmin:
        return fmgr::v1::ROLE_KIND_LAB_ADMIN;
      case core::RoleKind::Member:
        return fmgr::v1::ROLE_KIND_MEMBER;
      case core::RoleKind::ReadOnly:
        return fmgr::v1::ROLE_KIND_READ_ONLY;
      case core::RoleKind::ApiClient:
        return fmgr::v1::ROLE_KIND_API_CLIENT;
      }
      return fmgr::v1::ROLE_KIND_MEMBER;
    }

    [[nodiscard]] core::RoleKind from_proto_role_kind(fmgr::v1::RoleKind kind) {
      switch (kind) {
      case fmgr::v1::ROLE_KIND_SYSTEM_ADMIN:
        return core::RoleKind::SystemAdmin;
      case fmgr::v1::ROLE_KIND_LAB_ADMIN:
        return core::RoleKind::LabAdmin;
      case fmgr::v1::ROLE_KIND_MEMBER:
        return core::RoleKind::Member;
      case fmgr::v1::ROLE_KIND_READ_ONLY:
        return core::RoleKind::ReadOnly;
      case fmgr::v1::ROLE_KIND_API_CLIENT:
        return core::RoleKind::ApiClient;
      case fmgr::v1::ROLE_KIND_UNSPECIFIED:
      default:
        return core::RoleKind::Member;
      }
    }

    // parse_permission throws std::invalid_argument on an unknown key, which would
    // surface as INTERNAL. Translate to a ConstraintViolation so the client sees
    // INVALID_ARGUMENT for a bad permission_key.
    [[nodiscard]] core::Permission parse_permission_key(const std::string& key) {
      try {
        return core::parse_permission(key);
      } catch (const std::exception&) {
        throw storage::ConstraintViolation("unknown permission key: " + key);
      }
    }

    void fill_role(fmgr::v1::Role* out, const core::Role& role) {
      out->set_id(role.id.to_string());
      if (role.lab_id.has_value()) {
        out->set_lab_id(role.lab_id->to_string());
      }
      out->set_kind(to_proto_role_kind(role.kind));
      out->set_name(role.name);
      out->set_description(role.description);
      out->set_is_builtin(role.is_builtin);
      out->mutable_created_at()->set_unix_micros(role.created_at.unix_micros());
      if (role.archived_at.has_value()) {
        out->mutable_archived_at()->set_unix_micros(role.archived_at->unix_micros());
      }
    }

    // Read gate for entity-id-only RPCs: custom (lab-owned) roles require
    // UserManageRoles for the owning lab; global built-in role definitions are
    // readable by any authenticated principal.
    void gate_role_read(const auth::SessionContext& sctx, const core::Role& role) {
      if (role.lab_id.has_value() &&
          !sctx.has_for_lab(*role.lab_id, core::Permission::UserManageRoles)) {
        throw auth::PermissionDenied("user.manage_roles required for this lab");
      }
    }

  } // namespace

  RoleServiceImpl::RoleServiceImpl(auth::IAuthProvider& auth, storage::IStorageBackend& backend)
      : auth_(auth), backend_(backend), middleware_(auth) {
    using P = core::Permission;
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.RoleService/ListRoles", P::UserManageRoles);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.RoleService/GetRole", P::UserManageRoles);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.RoleService/CreateRole", P::UserManageRoles);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.RoleService/UpdateRole", P::UserManageRoles);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.RoleService/ArchiveRole", P::UserManageRoles);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.RoleService/ListRolePermissions",
                                      P::UserManageRoles);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.RoleService/GrantPermission", P::UserManageRoles);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.RoleService/RevokePermission", P::UserManageRoles);
  }

  // =====================================================================
  // Role
  // =====================================================================

  grpc::Status RoleServiceImpl::ListRoles(grpc::ServerContext* ctx,
                                          const fmgr::v1::ListRolesRequest* req,
                                          fmgr::v1::ListRolesResponse* resp) {
    try {
      if (!req->has_lab_id()) {
        return {grpc::StatusCode::INVALID_ARGUMENT, "lab_id is required"};
      }
      const auto lab_id = core::LabId::parse(req->lab_id());
      const auto sctx =
          middleware_.authorize(extract_bearer(*ctx), core::Permission::UserManageRoles, lab_id);

      auto query = storage::Query<core::Role>::all();
      if (req->include_archived()) {
        query = query.include_tombstoned();
      }

      auto txn = backend_.begin(storage::IsolationLevel::ReadCommitted);
      rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);
      const auto roles = txn->repo<core::Role>().query(query);
      txn->commit();

      // The Role repository query returns roles across all labs (no per-lab
      // predicate support); restrict to this lab's custom roles plus the global
      // built-in definitions, so a lab admin never sees another lab's roles.
      for (const auto& role : roles) {
        if (!role.lab_id.has_value() || *role.lab_id == lab_id) {
          fill_role(resp->add_roles(), role);
        }
      }
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

  grpc::Status RoleServiceImpl::GetRole(grpc::ServerContext* ctx,
                                        const fmgr::v1::GetRoleRequest* req,
                                        fmgr::v1::GetRoleResponse* resp) {
    try {
      const auto role_id = core::RoleId::parse(req->role_id());
      auto sctx = auth_.validate_token(extract_bearer(*ctx));
      if (!sctx.mfa_complete) {
        throw auth::MfaRequired("MFA required before this operation");
      }

      auto txn = backend_.begin(storage::IsolationLevel::ReadCommitted);
      rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);
      const auto role = txn->repo<core::Role>().find_by_id(role_id);
      txn->commit();

      if (!role.has_value() || role->archived_at.has_value()) {
        return {grpc::StatusCode::NOT_FOUND, "role not found"};
      }
      gate_role_read(sctx, *role);
      fill_role(resp->mutable_role(), *role);
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

  grpc::Status RoleServiceImpl::CreateRole(grpc::ServerContext* ctx,
                                           const fmgr::v1::CreateRoleRequest* req,
                                           fmgr::v1::CreateRoleResponse* resp) {
    try {
      const auto lab_id = core::LabId::parse(req->lab_id());
      const auto sctx =
          middleware_.authorize(extract_bearer(*ctx), core::Permission::UserManageRoles, lab_id);

      const core::Role role{
          .id = core::RoleId::parse(generate_uuid_v4()),
          .lab_id = lab_id,
          .kind = from_proto_role_kind(req->kind()),
          .name = req->name(),
          .description = req->description(),
          .is_builtin = false,
          .created_at = now_timestamp(),
      };

      auto txn = backend_.begin(storage::IsolationLevel::Serializable);
      rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);
      txn->repo<core::Role>().insert(role, make_ctx(sctx, "create_role"));
      txn->commit();

      fill_role(resp->mutable_role(), role);
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

  grpc::Status RoleServiceImpl::UpdateRole(grpc::ServerContext* ctx,
                                           const fmgr::v1::UpdateRoleRequest* req,
                                           fmgr::v1::UpdateRoleResponse* resp) {
    try {
      // A global built-in role carries no lab_id; it cannot be edited and there is
      // no lab to gate against.
      if (!req->role().has_lab_id()) {
        return {grpc::StatusCode::FAILED_PRECONDITION, "built-in roles cannot be modified"};
      }
      const auto lab_id = core::LabId::parse(req->role().lab_id());
      const auto role_id = core::RoleId::parse(req->role().id());
      const auto sctx =
          middleware_.authorize(extract_bearer(*ctx), core::Permission::UserManageRoles, lab_id);

      auto txn = backend_.begin(storage::IsolationLevel::Serializable);
      rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);
      auto existing = txn->repo<core::Role>().find_by_id(role_id);
      if (!existing.has_value() || existing->lab_id != lab_id ||
          existing->archived_at.has_value()) {
        return {grpc::StatusCode::NOT_FOUND, "role not found"};
      }
      if (existing->is_builtin) {
        return {grpc::StatusCode::FAILED_PRECONDITION, "built-in roles cannot be modified"};
      }
      // Mutable descriptive fields only; lab_id, is_builtin and timestamps are not
      // caller-editable.
      existing->kind = from_proto_role_kind(req->role().kind());
      existing->name = req->role().name();
      existing->description = req->role().description();
      txn->repo<core::Role>().update(*existing, make_ctx(sctx, "update_role"));
      txn->commit();

      fill_role(resp->mutable_role(), *existing);
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

  grpc::Status RoleServiceImpl::ArchiveRole(grpc::ServerContext* ctx,
                                            const fmgr::v1::ArchiveRoleRequest* req,
                                            fmgr::v1::ArchiveRoleResponse* /*resp*/) {
    try {
      const auto role_id = core::RoleId::parse(req->role_id());
      auto sctx = auth_.validate_token(extract_bearer(*ctx));
      if (!sctx.mfa_complete) {
        throw auth::MfaRequired("MFA required before this operation");
      }

      auto txn = backend_.begin(storage::IsolationLevel::Serializable);
      rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);
      const auto existing = txn->repo<core::Role>().find_by_id(role_id);
      if (!existing.has_value() || existing->archived_at.has_value()) {
        return {grpc::StatusCode::NOT_FOUND, "role not found"};
      }
      if (existing->is_builtin || !existing->lab_id.has_value()) {
        return {grpc::StatusCode::FAILED_PRECONDITION, "built-in roles cannot be archived"};
      }
      if (!sctx.has_for_lab(*existing->lab_id, core::Permission::UserManageRoles)) {
        throw auth::PermissionDenied("user.manage_roles required for this lab");
      }
      txn->repo<core::Role>().soft_delete(role_id, make_ctx(sctx, "archive_role"));
      txn->commit();
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

  // =====================================================================
  // RolePermission
  // =====================================================================

  grpc::Status RoleServiceImpl::ListRolePermissions(grpc::ServerContext* ctx,
                                                    const fmgr::v1::ListRolePermissionsRequest* req,
                                                    fmgr::v1::ListRolePermissionsResponse* resp) {
    try {
      const auto role_id = core::RoleId::parse(req->role_id());
      auto sctx = auth_.validate_token(extract_bearer(*ctx));
      if (!sctx.mfa_complete) {
        throw auth::MfaRequired("MFA required before this operation");
      }

      auto txn = backend_.begin(storage::IsolationLevel::ReadCommitted);
      rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);
      const auto role = txn->repo<core::Role>().find_by_id(role_id);
      if (!role.has_value() || role->archived_at.has_value()) {
        txn->commit();
        return {grpc::StatusCode::NOT_FOUND, "role not found"};
      }
      const auto grants =
          txn->repo<core::RolePermission>().query(storage::Query<core::RolePermission>::where(
              storage::field<core::RolePermission, std::string>(
                  core::RolePermission::Field::RoleId) == role_id.to_string()));
      txn->commit();

      gate_role_read(sctx, *role);
      for (const auto& grant : grants) {
        resp->add_permission_keys(std::string(core::to_key(grant.permission)));
      }
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

  grpc::Status RoleServiceImpl::GrantPermission(grpc::ServerContext* ctx,
                                                const fmgr::v1::GrantPermissionRequest* req,
                                                fmgr::v1::GrantPermissionResponse* /*resp*/) {
    try {
      const auto role_id = core::RoleId::parse(req->role_id());
      auto sctx = auth_.validate_token(extract_bearer(*ctx));
      if (!sctx.mfa_complete) {
        throw auth::MfaRequired("MFA required before this operation");
      }

      auto txn = backend_.begin(storage::IsolationLevel::Serializable);
      rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);
      const auto role = txn->repo<core::Role>().find_by_id(role_id);
      if (!role.has_value() || role->archived_at.has_value()) {
        return {grpc::StatusCode::NOT_FOUND, "role not found"};
      }
      if (role->is_builtin || !role->lab_id.has_value()) {
        return {grpc::StatusCode::FAILED_PRECONDITION,
                "built-in role permissions cannot be modified"};
      }
      if (!sctx.has_for_lab(*role->lab_id, core::Permission::UserManageRoles)) {
        throw auth::PermissionDenied("user.manage_roles required for this lab");
      }

      const core::RolePermission grant{.role_id = role_id,
                                       .permission = parse_permission_key(req->permission_key())};
      // Idempotent: granting an already-held permission is a no-op success.
      if (!txn->repo<core::RolePermission>().find_by_id(grant.id()).has_value()) {
        txn->repo<core::RolePermission>().insert(grant, make_ctx(sctx, "grant_permission"));
      }
      txn->commit();
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

  grpc::Status RoleServiceImpl::RevokePermission(grpc::ServerContext* ctx,
                                                 const fmgr::v1::RevokePermissionRequest* req,
                                                 fmgr::v1::RevokePermissionResponse* /*resp*/) {
    try {
      const auto role_id = core::RoleId::parse(req->role_id());
      auto sctx = auth_.validate_token(extract_bearer(*ctx));
      if (!sctx.mfa_complete) {
        throw auth::MfaRequired("MFA required before this operation");
      }

      auto txn = backend_.begin(storage::IsolationLevel::Serializable);
      rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);
      const auto role = txn->repo<core::Role>().find_by_id(role_id);
      if (!role.has_value() || role->archived_at.has_value()) {
        return {grpc::StatusCode::NOT_FOUND, "role not found"};
      }
      if (role->is_builtin || !role->lab_id.has_value()) {
        return {grpc::StatusCode::FAILED_PRECONDITION,
                "built-in role permissions cannot be modified"};
      }
      if (!sctx.has_for_lab(*role->lab_id, core::Permission::UserManageRoles)) {
        throw auth::PermissionDenied("user.manage_roles required for this lab");
      }

      const core::RolePermissionId grant_id{
          .role_id = role_id, .permission = parse_permission_key(req->permission_key())};
      // Idempotent: revoking a permission the role does not hold is a no-op success.
      if (txn->repo<core::RolePermission>().find_by_id(grant_id).has_value()) {
        txn->repo<core::RolePermission>().soft_delete(grant_id,
                                                      make_ctx(sctx, "revoke_permission"));
      }
      txn->commit();
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

} // namespace fmgr::server
