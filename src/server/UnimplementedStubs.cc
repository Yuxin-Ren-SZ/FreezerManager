// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Stub service implementations returning UNIMPLEMENTED.
// All RPC names are registered in the AuthMiddleware permission registry so
// the registry-completeness CI test passes. Handlers are filled in
// incrementally as each service is implemented.

#include "core/permissions.h"
#include "rpc/AuthMiddleware.h"

#include <fmgr/v1/audit.grpc.pb.h>
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
    static AuditServiceStub audit;
    static ShareServiceStub share;
    builder.RegisterService(&audit);
    builder.RegisterService(&share);
  }

} // namespace fmgr::server
