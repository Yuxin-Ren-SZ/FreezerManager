// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FMGR_SERVER_AUTHSERVICEIMPL_H
#define FMGR_SERVER_AUTHSERVICEIMPL_H

#include "auth/AuthTypes.h"
#include "auth/IAuthProvider.h"
#include "rpc/AuthMiddleware.h"
#include "rpc/RateLimiter.h"
#include "storage/IStorageBackend.h"

#include <fmgr/v1/auth.grpc.pb.h>
#include <grpcpp/grpcpp.h>

namespace fmgr::server {

  class AuthServiceImpl final : public fmgr::v1::AuthService::Service {
  public:
    // Default token-bucket budget for unauthenticated Login attempts per source
    // IP. Referenced by tests so the limit stays in lockstep with the handler.
    static constexpr double k_login_rate_capacity = 30.0;
    static constexpr double k_login_rate_refill_per_sec = 5.0;

    explicit AuthServiceImpl(auth::IAuthProvider& auth, storage::IStorageBackend& backend);

    grpc::Status Login(grpc::ServerContext* ctx, const fmgr::v1::LoginRequest* req,
                       fmgr::v1::LoginResponse* resp) override;

    grpc::Status SubmitMfa(grpc::ServerContext* ctx, const fmgr::v1::SubmitMfaRequest* req,
                           fmgr::v1::SubmitMfaResponse* resp) override;

    grpc::Status Logout(grpc::ServerContext* ctx, const fmgr::v1::LogoutRequest* req,
                        fmgr::v1::LogoutResponse* resp) override;

    grpc::Status CreateApiToken(grpc::ServerContext* ctx,
                                const fmgr::v1::CreateApiTokenRequest* req,
                                fmgr::v1::CreateApiTokenResponse* resp) override;

    grpc::Status ListApiTokens(grpc::ServerContext* ctx, const fmgr::v1::ListApiTokensRequest* req,
                               fmgr::v1::ListApiTokensResponse* resp) override;

    grpc::Status RevokeApiToken(grpc::ServerContext* ctx,
                                const fmgr::v1::RevokeApiTokenRequest* req,
                                fmgr::v1::RevokeApiTokenResponse* resp) override;

  private:
    auth::IAuthProvider& auth_;
    storage::IStorageBackend& backend_;
    rpc::AuthMiddleware middleware_;
    // Throttles unauthenticated Login attempts per source IP (audit H-1).
    rpc::RateLimiter login_limiter_;
  };

} // namespace fmgr::server

#endif // FMGR_SERVER_AUTHSERVICEIMPL_H
