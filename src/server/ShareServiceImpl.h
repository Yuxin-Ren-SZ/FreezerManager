// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FMGR_SERVER_SHARESERVICEIMPL_H
#define FMGR_SERVER_SHARESERVICEIMPL_H

#include "auth/IAuthProvider.h"
#include "rpc/AuthMiddleware.h"
#include "storage/IStorageBackend.h"

#include <fmgr/v1/share.grpc.pb.h>
#include <grpcpp/grpcpp.h>

namespace fmgr::server {

  // ShareService — the cross-lab share-request workflow (PRD D8).
  //
  // A ShareRequest runs a pending -> approved | rejected | revoked state machine.
  // Approval is the three-signature scheme: one ShareRequestApproval row per role
  // (SourceAdmin, TargetAdmin, SystemAdmin). The request becomes Approved only
  // once all three roles have signed; any rejection is terminal.
  //
  // The repository deliberately does NOT enforce the transitions (see
  // core/share_request.h); this service owns them.
  //
  // Gating model:
  //   - CreateShareRequest / RevokeShareRequest gate on ShareRequest for the
  //     source lab (the requesting lab).
  //   - Approve/Reject gate on the *approver_role*: a SourceLab signature requires
  //     ShareApprove for the source lab, a TargetLab signature requires it for the
  //     target lab, and a SystemAdmin signature requires a deployment SystemAdmin.
  //     This stops one lab forging another lab's signature.
  //   - List/Get are readable by a principal holding ShareRequest for either the
  //     source or target lab, or a deployment SystemAdmin.
  class ShareServiceImpl final : public fmgr::v1::ShareService::Service {
  public:
    explicit ShareServiceImpl(auth::IAuthProvider& auth, storage::IStorageBackend& backend);

    grpc::Status ListShareRequests(grpc::ServerContext* ctx,
                                   const fmgr::v1::ListShareRequestsRequest* req,
                                   fmgr::v1::ListShareRequestsResponse* resp) override;
    grpc::Status GetShareRequest(grpc::ServerContext* ctx,
                                 const fmgr::v1::GetShareRequestRequest* req,
                                 fmgr::v1::GetShareRequestResponse* resp) override;
    grpc::Status CreateShareRequest(grpc::ServerContext* ctx,
                                    const fmgr::v1::CreateShareRequestRequest* req,
                                    fmgr::v1::CreateShareRequestResponse* resp) override;
    grpc::Status ApproveShareRequest(grpc::ServerContext* ctx,
                                     const fmgr::v1::ApproveShareRequestRequest* req,
                                     fmgr::v1::ApproveShareRequestResponse* resp) override;
    grpc::Status RejectShareRequest(grpc::ServerContext* ctx,
                                    const fmgr::v1::RejectShareRequestRequest* req,
                                    fmgr::v1::RejectShareRequestResponse* resp) override;
    grpc::Status RevokeShareRequest(grpc::ServerContext* ctx,
                                    const fmgr::v1::RevokeShareRequestRequest* req,
                                    fmgr::v1::RevokeShareRequestResponse* resp) override;

  private:
    auth::IAuthProvider& auth_;
    storage::IStorageBackend& backend_;
    rpc::AuthMiddleware middleware_;
  };

} // namespace fmgr::server

#endif // FMGR_SERVER_SHARESERVICEIMPL_H
