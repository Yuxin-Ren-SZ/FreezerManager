// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Stub service implementations returning UNIMPLEMENTED.
// All RPC names are registered in the AuthMiddleware permission registry so
// the registry-completeness CI test passes. Handlers are filled in
// incrementally as each service is implemented.

#include "core/permissions.h"
#include "rpc/AuthMiddleware.h"

#include <fmgr/v1/audit.grpc.pb.h>
#include <fmgr/v1/box.grpc.pb.h>
#include <fmgr/v1/item_type.grpc.pb.h>
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

  // ---- BoxService stub ----

  class BoxServiceStub final : public fmgr::v1::BoxService::Service {
  public:
    BoxServiceStub() {
      using P = core::Permission;
      rpc::AuthMiddleware::register_rpc("/fmgr.v1.BoxService/ListFreezers", P::FreezerConfigure);
      rpc::AuthMiddleware::register_rpc("/fmgr.v1.BoxService/GetFreezer", P::FreezerConfigure);
      rpc::AuthMiddleware::register_rpc("/fmgr.v1.BoxService/CreateFreezer", P::FreezerConfigure);
      rpc::AuthMiddleware::register_rpc("/fmgr.v1.BoxService/UpdateFreezer", P::FreezerConfigure);
      rpc::AuthMiddleware::register_rpc("/fmgr.v1.BoxService/ArchiveFreezer", P::FreezerConfigure);
      rpc::AuthMiddleware::register_rpc("/fmgr.v1.BoxService/ListStorageContainers",
                                        P::FreezerConfigure);
      rpc::AuthMiddleware::register_rpc("/fmgr.v1.BoxService/CreateStorageContainer",
                                        P::FreezerConfigure);
      rpc::AuthMiddleware::register_rpc("/fmgr.v1.BoxService/UpdateStorageContainer",
                                        P::FreezerConfigure);
      rpc::AuthMiddleware::register_rpc("/fmgr.v1.BoxService/ArchiveStorageContainer",
                                        P::FreezerConfigure);
      rpc::AuthMiddleware::register_rpc("/fmgr.v1.BoxService/ListContainerTypes", P::BoxConfigure);
      rpc::AuthMiddleware::register_rpc("/fmgr.v1.BoxService/CreateContainerType", P::BoxConfigure);
      rpc::AuthMiddleware::register_rpc("/fmgr.v1.BoxService/ListBoxTypes", P::BoxConfigure);
      rpc::AuthMiddleware::register_rpc("/fmgr.v1.BoxService/CreateBoxType", P::BoxConfigure);
      rpc::AuthMiddleware::register_rpc("/fmgr.v1.BoxService/ListBoxes", P::SampleRead);
      rpc::AuthMiddleware::register_rpc("/fmgr.v1.BoxService/GetBox", P::SampleRead);
      rpc::AuthMiddleware::register_rpc("/fmgr.v1.BoxService/CreateBox", P::BoxConfigure);
      rpc::AuthMiddleware::register_rpc("/fmgr.v1.BoxService/UpdateBox", P::BoxConfigure);
      rpc::AuthMiddleware::register_rpc("/fmgr.v1.BoxService/ArchiveBox", P::BoxConfigure);
    }
    grpc::Status ListFreezers(grpc::ServerContext*, const fmgr::v1::ListFreezersRequest*,
                              fmgr::v1::ListFreezersResponse*) override {
      return unimplemented();
    }
    grpc::Status GetFreezer(grpc::ServerContext*, const fmgr::v1::GetFreezerRequest*,
                            fmgr::v1::GetFreezerResponse*) override {
      return unimplemented();
    }
    grpc::Status CreateFreezer(grpc::ServerContext*, const fmgr::v1::CreateFreezerRequest*,
                               fmgr::v1::CreateFreezerResponse*) override {
      return unimplemented();
    }
    grpc::Status UpdateFreezer(grpc::ServerContext*, const fmgr::v1::UpdateFreezerRequest*,
                               fmgr::v1::UpdateFreezerResponse*) override {
      return unimplemented();
    }
    grpc::Status ArchiveFreezer(grpc::ServerContext*, const fmgr::v1::ArchiveFreezerRequest*,
                                fmgr::v1::ArchiveFreezerResponse*) override {
      return unimplemented();
    }
    grpc::Status ListStorageContainers(grpc::ServerContext*,
                                       const fmgr::v1::ListStorageContainersRequest*,
                                       fmgr::v1::ListStorageContainersResponse*) override {
      return unimplemented();
    }
    grpc::Status CreateStorageContainer(grpc::ServerContext*,
                                        const fmgr::v1::CreateStorageContainerRequest*,
                                        fmgr::v1::CreateStorageContainerResponse*) override {
      return unimplemented();
    }
    grpc::Status UpdateStorageContainer(grpc::ServerContext*,
                                        const fmgr::v1::UpdateStorageContainerRequest*,
                                        fmgr::v1::UpdateStorageContainerResponse*) override {
      return unimplemented();
    }
    grpc::Status ArchiveStorageContainer(grpc::ServerContext*,
                                         const fmgr::v1::ArchiveStorageContainerRequest*,
                                         fmgr::v1::ArchiveStorageContainerResponse*) override {
      return unimplemented();
    }
    grpc::Status ListContainerTypes(grpc::ServerContext*,
                                    const fmgr::v1::ListContainerTypesRequest*,
                                    fmgr::v1::ListContainerTypesResponse*) override {
      return unimplemented();
    }
    grpc::Status CreateContainerType(grpc::ServerContext*,
                                     const fmgr::v1::CreateContainerTypeRequest*,
                                     fmgr::v1::CreateContainerTypeResponse*) override {
      return unimplemented();
    }
    grpc::Status ListBoxTypes(grpc::ServerContext*, const fmgr::v1::ListBoxTypesRequest*,
                              fmgr::v1::ListBoxTypesResponse*) override {
      return unimplemented();
    }
    grpc::Status CreateBoxType(grpc::ServerContext*, const fmgr::v1::CreateBoxTypeRequest*,
                               fmgr::v1::CreateBoxTypeResponse*) override {
      return unimplemented();
    }
    grpc::Status ListBoxes(grpc::ServerContext*, const fmgr::v1::ListBoxesRequest*,
                           fmgr::v1::ListBoxesResponse*) override {
      return unimplemented();
    }
    grpc::Status GetBox(grpc::ServerContext*, const fmgr::v1::GetBoxRequest*,
                        fmgr::v1::GetBoxResponse*) override {
      return unimplemented();
    }
    grpc::Status CreateBox(grpc::ServerContext*, const fmgr::v1::CreateBoxRequest*,
                           fmgr::v1::CreateBoxResponse*) override {
      return unimplemented();
    }
    grpc::Status UpdateBox(grpc::ServerContext*, const fmgr::v1::UpdateBoxRequest*,
                           fmgr::v1::UpdateBoxResponse*) override {
      return unimplemented();
    }
    grpc::Status ArchiveBox(grpc::ServerContext*, const fmgr::v1::ArchiveBoxRequest*,
                            fmgr::v1::ArchiveBoxResponse*) override {
      return unimplemented();
    }
  };

  // ---- ItemTypeService stub ----

  class ItemTypeServiceStub final : public fmgr::v1::ItemTypeService::Service {
  public:
    ItemTypeServiceStub() {
      using P = core::Permission;
      rpc::AuthMiddleware::register_rpc("/fmgr.v1.ItemTypeService/ListItemTypes",
                                        P::ItemTypeDefine);
      rpc::AuthMiddleware::register_rpc("/fmgr.v1.ItemTypeService/GetItemType", P::ItemTypeDefine);
      rpc::AuthMiddleware::register_rpc("/fmgr.v1.ItemTypeService/CreateItemType",
                                        P::ItemTypeDefine);
      rpc::AuthMiddleware::register_rpc("/fmgr.v1.ItemTypeService/UpdateItemType",
                                        P::ItemTypeDefine);
      rpc::AuthMiddleware::register_rpc("/fmgr.v1.ItemTypeService/ArchiveItemType",
                                        P::ItemTypeDefine);
      rpc::AuthMiddleware::register_rpc("/fmgr.v1.ItemTypeService/ListCustomFieldDefinitions",
                                        P::CustomFieldDefine);
      rpc::AuthMiddleware::register_rpc("/fmgr.v1.ItemTypeService/CreateCustomFieldDefinition",
                                        P::CustomFieldDefine);
      rpc::AuthMiddleware::register_rpc("/fmgr.v1.ItemTypeService/UpdateCustomFieldDefinition",
                                        P::CustomFieldDefine);
      rpc::AuthMiddleware::register_rpc("/fmgr.v1.ItemTypeService/ArchiveCustomFieldDefinition",
                                        P::CustomFieldDefine);
    }
    grpc::Status ListItemTypes(grpc::ServerContext*, const fmgr::v1::ListItemTypesRequest*,
                               fmgr::v1::ListItemTypesResponse*) override {
      return unimplemented();
    }
    grpc::Status GetItemType(grpc::ServerContext*, const fmgr::v1::GetItemTypeRequest*,
                             fmgr::v1::GetItemTypeResponse*) override {
      return unimplemented();
    }
    grpc::Status CreateItemType(grpc::ServerContext*, const fmgr::v1::CreateItemTypeRequest*,
                                fmgr::v1::CreateItemTypeResponse*) override {
      return unimplemented();
    }
    grpc::Status UpdateItemType(grpc::ServerContext*, const fmgr::v1::UpdateItemTypeRequest*,
                                fmgr::v1::UpdateItemTypeResponse*) override {
      return unimplemented();
    }
    grpc::Status ArchiveItemType(grpc::ServerContext*, const fmgr::v1::ArchiveItemTypeRequest*,
                                 fmgr::v1::ArchiveItemTypeResponse*) override {
      return unimplemented();
    }
    grpc::Status ListCustomFieldDefinitions(grpc::ServerContext*, const fmgr::v1::ListCfdsRequest*,
                                            fmgr::v1::ListCfdsResponse*) override {
      return unimplemented();
    }
    grpc::Status CreateCustomFieldDefinition(grpc::ServerContext*,
                                             const fmgr::v1::CreateCfdRequest*,
                                             fmgr::v1::CreateCfdResponse*) override {
      return unimplemented();
    }
    grpc::Status UpdateCustomFieldDefinition(grpc::ServerContext*,
                                             const fmgr::v1::UpdateCfdRequest*,
                                             fmgr::v1::UpdateCfdResponse*) override {
      return unimplemented();
    }
    grpc::Status ArchiveCustomFieldDefinition(grpc::ServerContext*,
                                              const fmgr::v1::ArchiveCfdRequest*,
                                              fmgr::v1::ArchiveCfdResponse*) override {
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
    static BoxServiceStub box;
    static ItemTypeServiceStub item_type;
    static RoleServiceStub role;
    static AuditServiceStub audit;
    static ShareServiceStub share;
    builder.RegisterService(&sample);
    builder.RegisterService(&box);
    builder.RegisterService(&item_type);
    builder.RegisterService(&role);
    builder.RegisterService(&audit);
    builder.RegisterService(&share);
  }

} // namespace fmgr::server
