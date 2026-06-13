// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FMGR_SERVER_ROLESERVICEIMPL_H
#define FMGR_SERVER_ROLESERVICEIMPL_H

#include "auth/IAuthProvider.h"
#include "rpc/AuthMiddleware.h"
#include "storage/IStorageBackend.h"

#include <fmgr/v1/role.grpc.pb.h>
#include <grpcpp/grpcpp.h>

namespace fmgr::server {

  // RoleService — the RBAC administration vertical. Every RPC gates on
  // Permission::UserManageRoles, which is lab-scoped (never global — see
  // core::is_global_only_permission).
  //
  // Two resources with a single permission gate:
  //   - Role: a self-contained entity. Built-in roles (SystemAdmin .. ApiClient,
  //     fixed UUIDs) are GLOBAL (lab_id is null) and IMMUTABLE; custom lab roles
  //     carry a real lab_id.
  //   - RolePermission: the (role, permission) grant join, mutated by
  //     Grant/RevokePermission.
  //
  // Gating model:
  //   - List*/Create*/Update* carry the lab id in the request and gate up-front
  //     via AuthMiddleware::authorize.
  //   - Get*/Archive*/ListRolePermissions/Grant*/Revoke* carry only an entity id;
  //     they validate the token, load the row, then resolve the gate against the
  //     role's owning lab. A global built-in role is world-readable to any valid
  //     token but cannot be written (FAILED_PRECONDITION).
  class RoleServiceImpl final : public fmgr::v1::RoleService::Service {
  public:
    explicit RoleServiceImpl(auth::IAuthProvider& auth, storage::IStorageBackend& backend);

    // ---- Role ----
    grpc::Status ListRoles(grpc::ServerContext* ctx, const fmgr::v1::ListRolesRequest* req,
                           fmgr::v1::ListRolesResponse* resp) override;
    grpc::Status GetRole(grpc::ServerContext* ctx, const fmgr::v1::GetRoleRequest* req,
                         fmgr::v1::GetRoleResponse* resp) override;
    grpc::Status CreateRole(grpc::ServerContext* ctx, const fmgr::v1::CreateRoleRequest* req,
                            fmgr::v1::CreateRoleResponse* resp) override;
    grpc::Status UpdateRole(grpc::ServerContext* ctx, const fmgr::v1::UpdateRoleRequest* req,
                            fmgr::v1::UpdateRoleResponse* resp) override;
    grpc::Status ArchiveRole(grpc::ServerContext* ctx, const fmgr::v1::ArchiveRoleRequest* req,
                             fmgr::v1::ArchiveRoleResponse* resp) override;

    // ---- RolePermission ----
    grpc::Status ListRolePermissions(grpc::ServerContext* ctx,
                                     const fmgr::v1::ListRolePermissionsRequest* req,
                                     fmgr::v1::ListRolePermissionsResponse* resp) override;
    grpc::Status GrantPermission(grpc::ServerContext* ctx,
                                 const fmgr::v1::GrantPermissionRequest* req,
                                 fmgr::v1::GrantPermissionResponse* resp) override;
    grpc::Status RevokePermission(grpc::ServerContext* ctx,
                                  const fmgr::v1::RevokePermissionRequest* req,
                                  fmgr::v1::RevokePermissionResponse* resp) override;

  private:
    auth::IAuthProvider& auth_;
    storage::IStorageBackend& backend_;
    rpc::AuthMiddleware middleware_;
  };

} // namespace fmgr::server

#endif // FMGR_SERVER_ROLESERVICEIMPL_H
