// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FMGR_SERVER_SESSIONSERVICEIMPL_H
#define FMGR_SERVER_SESSIONSERVICEIMPL_H

#include "auth/IAuthProvider.h"
#include "rpc/AuthMiddleware.h"
#include "storage/IStorageBackend.h"

#include <fmgr/v1/session.grpc.pb.h>
#include <grpcpp/grpcpp.h>

namespace fmgr::server {

  class SessionServiceImpl final : public fmgr::v1::SessionService::Service {
  public:
    explicit SessionServiceImpl(auth::IAuthProvider& auth, storage::IStorageBackend& backend);

    grpc::Status ListSessions(grpc::ServerContext* ctx, const fmgr::v1::ListSessionsRequest* req,
                              fmgr::v1::ListSessionsResponse* resp) override;

    grpc::Status RevokeSession(grpc::ServerContext* ctx, const fmgr::v1::RevokeSessionRequest* req,
                               fmgr::v1::RevokeSessionResponse* resp) override;

  private:
    auth::IAuthProvider& auth_;
    storage::IStorageBackend& backend_;
    rpc::AuthMiddleware middleware_;
  };

} // namespace fmgr::server

#endif // FMGR_SERVER_SESSIONSERVICEIMPL_H
