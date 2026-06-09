// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FMGR_SERVER_LABSERVICEIMPL_H
#define FMGR_SERVER_LABSERVICEIMPL_H

#include "auth/IAuthProvider.h"
#include "rpc/AuthMiddleware.h"
#include "storage/IStorageBackend.h"

#include <fmgr/v1/lab.grpc.pb.h>
#include <grpcpp/grpcpp.h>

namespace fmgr::server {

  // LabService — first service to exercise the RBAC gate (AuthMiddleware::authorize)
  // rather than the self-management validate_token() path used by AuthService.
  //
  // CreateLab is gated by the global-only Permission::LabProvision (a SystemAdmin
  // deployment action): a brand-new lab has no membership yet, so no lab-scoped
  // permission can apply. Every other RPC is lab-scoped against the lab in the
  // request.
  class LabServiceImpl final : public fmgr::v1::LabService::Service {
  public:
    explicit LabServiceImpl(auth::IAuthProvider& auth, storage::IStorageBackend& backend);

    grpc::Status GetLab(grpc::ServerContext* ctx, const fmgr::v1::GetLabRequest* req,
                        fmgr::v1::GetLabResponse* resp) override;

    grpc::Status ListLabs(grpc::ServerContext* ctx, const fmgr::v1::ListLabsRequest* req,
                          fmgr::v1::ListLabsResponse* resp) override;

    grpc::Status CreateLab(grpc::ServerContext* ctx, const fmgr::v1::CreateLabRequest* req,
                           fmgr::v1::CreateLabResponse* resp) override;

    grpc::Status UpdateLab(grpc::ServerContext* ctx, const fmgr::v1::UpdateLabRequest* req,
                           fmgr::v1::UpdateLabResponse* resp) override;

    grpc::Status EnablePhi(grpc::ServerContext* ctx, const fmgr::v1::EnablePhiRequest* req,
                           fmgr::v1::EnablePhiResponse* resp) override;

    grpc::Status ListMembers(grpc::ServerContext* ctx, const fmgr::v1::ListMembersRequest* req,
                             fmgr::v1::ListMembersResponse* resp) override;

    grpc::Status InviteMember(grpc::ServerContext* ctx, const fmgr::v1::InviteMemberRequest* req,
                              fmgr::v1::InviteMemberResponse* resp) override;

    grpc::Status RevokeMembership(grpc::ServerContext* ctx,
                                  const fmgr::v1::RevokeMembershipRequest* req,
                                  fmgr::v1::RevokeMembershipResponse* resp) override;

  private:
    auth::IAuthProvider& auth_;
    storage::IStorageBackend& backend_;
    rpc::AuthMiddleware middleware_;
  };

} // namespace fmgr::server

#endif // FMGR_SERVER_LABSERVICEIMPL_H
