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

    // Brute-force budget for SubmitMfa, keyed by user rather than by session or
    // token: a six-digit TOTP is guessable at machine speed, and re-logging in
    // with the same stolen password must not mint a fresh set of guesses (#128).
    // Five immediate attempts cover a mistyped code plus retries; past that the
    // budget refills one attempt per minute. Referenced by tests so the limit
    // stays in lockstep with the handler.
    static constexpr double k_mfa_rate_capacity = 5.0;
    static constexpr double k_mfa_rate_refill_per_sec = 1.0 / 60.0;

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

    // The caller's own identity, memberships, roles and effective permission
    // keys (#140). Takes no subject: the bearer token is the only thing that
    // decides who is described.
    grpc::Status WhoAmI(grpc::ServerContext* ctx, const fmgr::v1::WhoAmIRequest* req,
                        fmgr::v1::WhoAmIResponse* resp) override;

  private:
    auth::IAuthProvider& auth_;
    storage::IStorageBackend& backend_;
    rpc::AuthMiddleware middleware_;
    // Throttles unauthenticated Login attempts per source IP (audit H-1).
    rpc::RateLimiter login_limiter_;
    // Throttles TOTP submissions per user (#128): Login's per-IP budget does not
    // cover guessing a six-digit code through an already-issued pending session.
    rpc::RateLimiter mfa_limiter_;
  };

} // namespace fmgr::server

#endif // FMGR_SERVER_AUTHSERVICEIMPL_H
