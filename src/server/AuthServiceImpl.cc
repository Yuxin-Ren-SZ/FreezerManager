// SPDX-License-Identifier: AGPL-3.0-or-later

#include "server/AuthServiceImpl.h"

#include "core/permissions.h"
#include "core/session.h"
#include "server/GrpcErrorTranslation.h"
#include "storage/IStorageBackend.h"
#include "storage/SessionTraits.h"

#include <fmgr/v1/auth.grpc.pb.h>
#include <grpcpp/grpcpp.h>

#include <chrono>
#include <optional>
#include <string>

namespace fmgr::server {
  namespace {

    // Validate token and enforce MFA gate without a RBAC check.
    // Used for "self-management" RPCs (Logout, CreateApiToken, etc.) that any
    // authenticated user may call for their own data.
    [[nodiscard]] auth::SessionContext validate_authed(auth::IAuthProvider& auth,
                                                       const grpc::ServerContext& ctx) {
      const auto bearer = extract_bearer(ctx);
      auto sctx = auth.validate_token(bearer);
      if (!sctx.mfa_complete) {
        throw auth::MfaRequired("MFA required before this operation");
      }
      return sctx;
    }

    [[nodiscard]] storage::MutationContext make_ctx(const auth::SessionContext& sctx,
                                                    std::string_view reason) {
      return storage::MutationContext{
          .actor_user_id = sctx.user_id,
          .actor_session_id = sctx.session_id.to_string(),
          .request_id = "",
          .reason = std::string(reason),
      };
    }

    void fill_api_token_summary(fmgr::v1::ApiTokenSummary* out, const core::ApiToken& tok) {
      out->set_id(tok.id.to_string());
      out->set_user_id(tok.user_id.to_string());
      if (tok.lab_id.has_value()) {
        out->set_lab_id(tok.lab_id->to_string());
      }
      out->set_name(tok.name);
      out->set_scope_json(tok.scope_json);
      out->set_token_prefix(tok.token_prefix);
      out->mutable_created_at()->set_unix_micros(tok.created_at.unix_micros());
      if (tok.expires_at.has_value()) {
        out->mutable_expires_at()->set_unix_micros(tok.expires_at->unix_micros());
      }
      if (tok.revoked_at.has_value()) {
        out->mutable_revoked_at()->set_unix_micros(tok.revoked_at->unix_micros());
      }
    }

  } // namespace

  AuthServiceImpl::AuthServiceImpl(auth::IAuthProvider& auth, storage::IStorageBackend& backend)
      : auth_(auth), backend_(backend), middleware_(auth) {
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.AuthService/Login",
                                      core::Permission::SessionRevoke);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.AuthService/SubmitMfa",
                                      core::Permission::SessionRevoke);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.AuthService/Logout",
                                      core::Permission::SessionRevoke);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.AuthService/CreateApiToken",
                                      core::Permission::SessionRevoke);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.AuthService/ListApiTokens",
                                      core::Permission::SessionRevoke);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.AuthService/RevokeApiToken",
                                      core::Permission::SessionRevoke);
  }

  grpc::Status AuthServiceImpl::Login(grpc::ServerContext* /*ctx*/,
                                      const fmgr::v1::LoginRequest* req,
                                      fmgr::v1::LoginResponse* resp) {
    try {
      const auth::AuthCredentials creds = auth::PasswordCredentials{
          .email = req->email(),
          .password = req->password(),
      };
      const auto token = auth_.authenticate(creds, {});
      resp->set_session_token(token.plaintext_token);
      resp->set_session_id(token.session_id.to_string());
      resp->set_mfa_required(!token.mfa_complete);
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

  grpc::Status AuthServiceImpl::SubmitMfa(grpc::ServerContext* ctx,
                                          const fmgr::v1::SubmitMfaRequest* req,
                                          fmgr::v1::SubmitMfaResponse* /*resp*/) {
    try {
      const auto bearer = extract_bearer(*ctx);
      const auto sctx = auth_.validate_token(bearer);
      auth_.verify_totp(sctx.session_id, req->totp_code());
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

  grpc::Status AuthServiceImpl::Logout(grpc::ServerContext* ctx,
                                       const fmgr::v1::LogoutRequest* /*req*/,
                                       fmgr::v1::LogoutResponse* /*resp*/) {
    try {
      const auto sctx = validate_authed(auth_, *ctx);
      auth_.revoke_session(sctx.session_id, make_ctx(sctx, "logout"));
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

  grpc::Status AuthServiceImpl::CreateApiToken(grpc::ServerContext* ctx,
                                               const fmgr::v1::CreateApiTokenRequest* req,
                                               fmgr::v1::CreateApiTokenResponse* resp) {
    try {
      const auto sctx = validate_authed(auth_, *ctx);

      std::optional<core::LabId> lab_id;
      if (req->has_lab_id()) {
        lab_id = core::LabId::parse(req->lab_id());
      }

      std::optional<core::Timestamp> expires_at;
      if (req->expires_in_days() > 0) {
        const auto now_micros = std::chrono::duration_cast<std::chrono::microseconds>(
                                    std::chrono::system_clock::now().time_since_epoch())
                                    .count();
        expires_at = core::Timestamp::from_unix_micros(
            now_micros + static_cast<std::int64_t>(req->expires_in_days()) * 86400LL * 1000000LL);
      }

      const auto ctx_mut = make_ctx(sctx, "create_api_token");
      const auto result = auth_.create_api_token(sctx.user_id, req->name(), req->scope_json(),
                                                 lab_id, expires_at, ctx_mut);

      resp->set_token(result.plaintext_token);
      resp->set_api_token_id(result.api_token_id.to_string());
      resp->set_token_prefix(result.token_prefix);
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

  grpc::Status AuthServiceImpl::ListApiTokens(grpc::ServerContext* ctx,
                                              const fmgr::v1::ListApiTokensRequest* req,
                                              fmgr::v1::ListApiTokensResponse* resp) {
    try {
      const auto sctx = validate_authed(auth_, *ctx);

      auto txn = backend_.begin(storage::IsolationLevel::ReadCommitted);
      rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);

      auto query = storage::Query<core::ApiToken>::where(
          storage::field<core::ApiToken, std::string>(core::ApiToken::Field::UserId) ==
          sctx.user_id.to_string());
      if (req->include_revoked()) {
        query = query.include_tombstoned();
      }

      const auto tokens = txn->repo<core::ApiToken>().query(query);
      txn->commit();

      for (const auto& tok : tokens) {
        fill_api_token_summary(resp->add_tokens(), tok);
      }
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

  grpc::Status AuthServiceImpl::RevokeApiToken(grpc::ServerContext* ctx,
                                               const fmgr::v1::RevokeApiTokenRequest* req,
                                               fmgr::v1::RevokeApiTokenResponse* /*resp*/) {
    try {
      const auto sctx = validate_authed(auth_, *ctx);
      const auto api_token_id = core::ApiTokenId::parse(req->api_token_id());
      auth_.revoke_api_token(api_token_id, make_ctx(sctx, "revoke_api_token"));
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

} // namespace fmgr::server
