// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Stub service implementations returning UNIMPLEMENTED.
// All RPC names are registered in the AuthMiddleware permission registry so
// the registry-completeness CI test passes. Handlers are filled in
// incrementally as each service is implemented.

#include "core/permissions.h"
#include "rpc/AuthMiddleware.h"

#include <fmgr/v1/audit.grpc.pb.h>
#include <fmgr/v1/role.grpc.pb.h>
#include <fmgr/v1/sample.grpc.pb.h>
#include <fmgr/v1/share.grpc.pb.h>
#include <grpcpp/grpcpp.h>

#include <string_view>

namespace fmgr::server {

  // Helper for stub methods.
  namespace {
    [[nodiscard]] grpc::Status unimplemented() {
      return {grpc::StatusCode::UNIMPLEMENTED, "not yet implemented"};
    }
  } // namespace

  // Stub overrides intentionally leave the (ctx, request, response) parameters
  // unnamed — they are placeholders until each service is implemented.
  // NOLINTBEGIN(readability-named-parameter)

  // ---- SampleService stub ----

  class SampleServiceStub final : public fmgr::v1::SampleService::Service {
  public:
    SampleServiceStub() {
      using P = core::Permission;
      rpc::AuthMiddleware::register_rpc("/fmgr.v1.SampleService/ListSamples", P::SampleRead);
      rpc::AuthMiddleware::register_rpc("/fmgr.v1.SampleService/GetSample", P::SampleRead);
      rpc::AuthMiddleware::register_rpc("/fmgr.v1.SampleService/CreateSample", P::SampleWrite);
      rpc::AuthMiddleware::register_rpc("/fmgr.v1.SampleService/UpdateSample", P::SampleWrite);
      rpc::AuthMiddleware::register_rpc("/fmgr.v1.SampleService/SoftDeleteSample",
                                        P::SampleDeleteSoft);
      rpc::AuthMiddleware::register_rpc("/fmgr.v1.SampleService/MoveSample", P::SampleWrite);
      rpc::AuthMiddleware::register_rpc("/fmgr.v1.SampleService/CheckoutSample", P::SampleCheckout);
      rpc::AuthMiddleware::register_rpc("/fmgr.v1.SampleService/ExportSamplesCsv", P::SampleRead);
    }
    grpc::Status ListSamples(grpc::ServerContext*, const fmgr::v1::ListSamplesRequest*,
                             fmgr::v1::ListSamplesResponse*) override {
      return unimplemented();
    }
    grpc::Status GetSample(grpc::ServerContext*, const fmgr::v1::GetSampleRequest*,
                           fmgr::v1::GetSampleResponse*) override {
      return unimplemented();
    }
    grpc::Status CreateSample(grpc::ServerContext*, const fmgr::v1::CreateSampleRequest*,
                              fmgr::v1::CreateSampleResponse*) override {
      return unimplemented();
    }
    grpc::Status UpdateSample(grpc::ServerContext*, const fmgr::v1::UpdateSampleRequest*,
                              fmgr::v1::UpdateSampleResponse*) override {
      return unimplemented();
    }
    grpc::Status SoftDeleteSample(grpc::ServerContext*, const fmgr::v1::SoftDeleteSampleRequest*,
                                  fmgr::v1::SoftDeleteSampleResponse*) override {
      return unimplemented();
    }
    grpc::Status MoveSample(grpc::ServerContext*, const fmgr::v1::MoveSampleRequest*,
                            fmgr::v1::MoveSampleResponse*) override {
      return unimplemented();
    }
    grpc::Status CheckoutSample(grpc::ServerContext*, const fmgr::v1::CheckoutSampleRequest*,
                                fmgr::v1::CheckoutSampleResponse*) override {
      return unimplemented();
    }
    grpc::Status ExportSamplesCsv(grpc::ServerContext*, const fmgr::v1::ExportSamplesCsvRequest*,
                                  fmgr::v1::ExportSamplesCsvResponse*) override {
      return unimplemented();
    }
  };

  // ---- RoleService stub ----

  class RoleServiceStub final : public fmgr::v1::RoleService::Service {
  public:
    RoleServiceStub() {
      using P = core::Permission;
      rpc::AuthMiddleware::register_rpc("/fmgr.v1.RoleService/ListRoles", P::UserManageRoles);
      rpc::AuthMiddleware::register_rpc("/fmgr.v1.RoleService/GetRole", P::UserManageRoles);
      rpc::AuthMiddleware::register_rpc("/fmgr.v1.RoleService/CreateRole", P::UserManageRoles);
      rpc::AuthMiddleware::register_rpc("/fmgr.v1.RoleService/UpdateRole", P::UserManageRoles);
      rpc::AuthMiddleware::register_rpc("/fmgr.v1.RoleService/ArchiveRole", P::UserManageRoles);
      rpc::AuthMiddleware::register_rpc("/fmgr.v1.RoleService/ListRolePermissions",
                                        P::UserManageRoles);
      rpc::AuthMiddleware::register_rpc("/fmgr.v1.RoleService/GrantPermission", P::UserManageRoles);
      rpc::AuthMiddleware::register_rpc("/fmgr.v1.RoleService/RevokePermission",
                                        P::UserManageRoles);
    }
    grpc::Status ListRoles(grpc::ServerContext*, const fmgr::v1::ListRolesRequest*,
                           fmgr::v1::ListRolesResponse*) override {
      return unimplemented();
    }
    grpc::Status GetRole(grpc::ServerContext*, const fmgr::v1::GetRoleRequest*,
                         fmgr::v1::GetRoleResponse*) override {
      return unimplemented();
    }
    grpc::Status CreateRole(grpc::ServerContext*, const fmgr::v1::CreateRoleRequest*,
                            fmgr::v1::CreateRoleResponse*) override {
      return unimplemented();
    }
    grpc::Status UpdateRole(grpc::ServerContext*, const fmgr::v1::UpdateRoleRequest*,
                            fmgr::v1::UpdateRoleResponse*) override {
      return unimplemented();
    }
    grpc::Status ArchiveRole(grpc::ServerContext*, const fmgr::v1::ArchiveRoleRequest*,
                             fmgr::v1::ArchiveRoleResponse*) override {
      return unimplemented();
    }
    grpc::Status ListRolePermissions(grpc::ServerContext*,
                                     const fmgr::v1::ListRolePermissionsRequest*,
                                     fmgr::v1::ListRolePermissionsResponse*) override {
      return unimplemented();
    }
    grpc::Status GrantPermission(grpc::ServerContext*, const fmgr::v1::GrantPermissionRequest*,
                                 fmgr::v1::GrantPermissionResponse*) override {
      return unimplemented();
    }
    grpc::Status RevokePermission(grpc::ServerContext*, const fmgr::v1::RevokePermissionRequest*,
                                  fmgr::v1::RevokePermissionResponse*) override {
      return unimplemented();
    }
  };

  // ---- AuditService stub ----

  class AuditServiceStub final : public fmgr::v1::AuditService::Service {
  public:
    AuditServiceStub() {
      using P = core::Permission;
      rpc::AuthMiddleware::register_rpc("/fmgr.v1.AuditService/ListAuditEvents", P::AuditRead);
      rpc::AuthMiddleware::register_rpc("/fmgr.v1.AuditService/GetAuditEvent", P::AuditRead);
      rpc::AuthMiddleware::register_rpc("/fmgr.v1.AuditService/VerifyAuditChain", P::AuditRead);
      rpc::AuthMiddleware::register_rpc("/fmgr.v1.AuditService/ExportAuditLog", P::AuditExport);
    }
    grpc::Status ListAuditEvents(grpc::ServerContext*, const fmgr::v1::ListAuditEventsRequest*,
                                 fmgr::v1::ListAuditEventsResponse*) override {
      return unimplemented();
    }
    grpc::Status GetAuditEvent(grpc::ServerContext*, const fmgr::v1::GetAuditEventRequest*,
                               fmgr::v1::GetAuditEventResponse*) override {
      return unimplemented();
    }
    grpc::Status VerifyAuditChain(grpc::ServerContext*, const fmgr::v1::VerifyAuditChainRequest*,
                                  fmgr::v1::VerifyAuditChainResponse*) override {
      return unimplemented();
    }
    grpc::Status ExportAuditLog(grpc::ServerContext*, const fmgr::v1::ExportAuditLogRequest*,
                                fmgr::v1::ExportAuditLogResponse*) override {
      return unimplemented();
    }
  };

  // ---- ShareService stub ----

  class ShareServiceStub final : public fmgr::v1::ShareService::Service {
  public:
    ShareServiceStub() {
      using P = core::Permission;
      rpc::AuthMiddleware::register_rpc("/fmgr.v1.ShareService/ListShareRequests", P::ShareRequest);
      rpc::AuthMiddleware::register_rpc("/fmgr.v1.ShareService/GetShareRequest", P::ShareRequest);
      rpc::AuthMiddleware::register_rpc("/fmgr.v1.ShareService/CreateShareRequest",
                                        P::ShareRequest);
      rpc::AuthMiddleware::register_rpc("/fmgr.v1.ShareService/ApproveShareRequest",
                                        P::ShareApprove);
      rpc::AuthMiddleware::register_rpc("/fmgr.v1.ShareService/RejectShareRequest",
                                        P::ShareApprove);
      rpc::AuthMiddleware::register_rpc("/fmgr.v1.ShareService/RevokeShareRequest",
                                        P::ShareRequest);
    }
    grpc::Status ListShareRequests(grpc::ServerContext*, const fmgr::v1::ListShareRequestsRequest*,
                                   fmgr::v1::ListShareRequestsResponse*) override {
      return unimplemented();
    }
    grpc::Status GetShareRequest(grpc::ServerContext*, const fmgr::v1::GetShareRequestRequest*,
                                 fmgr::v1::GetShareRequestResponse*) override {
      return unimplemented();
    }
    grpc::Status CreateShareRequest(grpc::ServerContext*,
                                    const fmgr::v1::CreateShareRequestRequest*,
                                    fmgr::v1::CreateShareRequestResponse*) override {
      return unimplemented();
    }
    grpc::Status ApproveShareRequest(grpc::ServerContext*,
                                     const fmgr::v1::ApproveShareRequestRequest*,
                                     fmgr::v1::ApproveShareRequestResponse*) override {
      return unimplemented();
    }
    grpc::Status RejectShareRequest(grpc::ServerContext*,
                                    const fmgr::v1::RejectShareRequestRequest*,
                                    fmgr::v1::RejectShareRequestResponse*) override {
      return unimplemented();
    }
    grpc::Status RevokeShareRequest(grpc::ServerContext*,
                                    const fmgr::v1::RevokeShareRequestRequest*,
                                    fmgr::v1::RevokeShareRequestResponse*) override {
      return unimplemented();
    }
  };

  // NOLINTEND(readability-named-parameter)

  // ---- Registration function ----
  //
  // Called from FreezerServer constructor to register stub services.
  // The stub objects outlive the server (static storage).

  void register_stub_services(grpc::ServerBuilder& builder) {
    static SampleServiceStub sample;
    static RoleServiceStub role;
    static AuditServiceStub audit;
    static ShareServiceStub share;
    builder.RegisterService(&sample);
    builder.RegisterService(&role);
    builder.RegisterService(&audit);
    builder.RegisterService(&share);
  }

} // namespace fmgr::server
