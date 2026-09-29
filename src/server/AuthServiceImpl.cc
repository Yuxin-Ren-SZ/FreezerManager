// SPDX-License-Identifier: AGPL-3.0-or-later

#include "server/AuthServiceImpl.h"
#include "server/RequestId.h"

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

    // Derive a per-source-IP rate-limit key from the gRPC peer string, dropping
    // the ephemeral port so all connections from one host share a bucket.
    // Peer looks like "ipv4:1.2.3.4:54321" or "ipv6:[::1]:54321".
    [[nodiscard]] std::string peer_ip_key(const grpc::ServerContext& ctx) {
      std::string peer = ctx.peer();
      const auto last_colon = peer.rfind(':');
      if (last_colon == std::string::npos) {
        return peer;
      }
      return peer.substr(0, last_colon);
    }

    [[nodiscard]] storage::MutationContext make_ctx(const grpc::ServerContext& ctx,
                                                    const auth::SessionContext& sctx,
                                                    std::string_view reason) {
      return storage::MutationContext{
          .actor_user_id = sctx.user_id,
          .actor_session_id = sctx.session_id.to_string(),
          .request_id = request_id_from(ctx),
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
      : auth_(auth), backend_(backend), middleware_(auth),
        login_limiter_(rpc::RateLimiterConfig{.capacity = k_login_rate_capacity,
                                              .refill_per_sec = k_login_rate_refill_per_sec}) {
    // #78/#119: none of the auth/* RPCs is permission-gated, and session.revoke —
    // the permission all six used to name — is checked by no code path here. Each
    // now registers the credential rule its handler applies, and the handler asks
    // the gate for that same rule, so the gate applies it and refuses the call if
    // the two ever diverge. Login needs no credential at all; SubmitMfa needs a
    // session token but deliberately not MFA; Logout likewise (#62 — a pending-MFA
    // session must be able to give the credential up); the rest need a token whose
    // second factor is complete.
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.AuthService/Login", rpc::RpcGate::no_credential());
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.AuthService/SubmitMfa",
                                      rpc::RpcGate::token_only());
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.AuthService/Logout", rpc::RpcGate::token_only());
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.AuthService/CreateApiToken",
                                      rpc::RpcGate::token_and_mfa());
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.AuthService/ListApiTokens",
                                      rpc::RpcGate::token_and_mfa());
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.AuthService/RevokeApiToken",
                                      rpc::RpcGate::token_and_mfa());
  }

  grpc::Status AuthServiceImpl::Login(grpc::ServerContext* ctx, const fmgr::v1::LoginRequest* req,
                                      fmgr::v1::LoginResponse* resp) {
    // Declared anonymous: a caller logging in has no credential yet. The RPC
    // identity is read rather than the Authorization header because this handler
    // must not start refusing callers for a header it never reads; the gate call
    // is what ties the handler to its registration, so a registration that ever
    // claimed a credential rule would refuse every login rather than widen
    // silently (#119).
    middleware_.admit_no_credential(
        rpc::RpcCall{.bearer_token = {}, .method = rpc_method_name(*ctx)});
    // Throttle by source IP before doing any work (notably the expensive
    // Argon2id verify). Caps credential-spray / account-enumeration volume that
    // the per-email lockout cannot (audit H-1).
    if (!login_limiter_.try_acquire(peer_ip_key(*ctx), rpc::RateLimiter::Clock::now())) {
      return {grpc::StatusCode::RESOURCE_EXHAUSTED, "too many login attempts; slow down"};
    }
    try {
      const auth::AuthCredentials creds = auth::PasswordCredentials{
          .email = req->email(),
          .password = req->password(),
      };
      const auto token = auth_.authenticate(creds, {});
      resp->set_session_token(token.plaintext_token);
      resp->set_session_id(token.session_id.to_string());
      resp->set_user_id(token.user_id.to_string());
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
      // A token, deliberately without MFA: completing the second factor is what
      // this RPC does, so requiring it here would deadlock every login (#62).
      const auto sctx =
          middleware_.authenticate(extract_bearer(*ctx), rpc::CredentialRule::TokenOnly);
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
      // Deliberately token-only, not token-and-MFA: the browser login route sets
      // the session cookie before the second factor is entered, so a pending-MFA
      // session is a credential the UI holds. Logout only removes authority, so
      // refusing it would leave an abandoned login with a cookie nothing can
      // revoke (`SameSite=Strict` keeps every other page from clearing it).
      // SubmitMfa is the other half of that exception; together they are the two
      // RPCs that declare CredentialRule::TokenOnly.
      const auto sctx =
          middleware_.authenticate(extract_bearer(*ctx), rpc::CredentialRule::TokenOnly);
      auth_.revoke_session(sctx.session_id, make_ctx(*ctx, sctx, "logout"));
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

  grpc::Status AuthServiceImpl::CreateApiToken(grpc::ServerContext* ctx,
                                               const fmgr::v1::CreateApiTokenRequest* req,
                                               fmgr::v1::CreateApiTokenResponse* resp) {
    try {
      const auto sctx =
          middleware_.authenticate(extract_bearer(*ctx), rpc::CredentialRule::TokenAndMfa);

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

      const auto ctx_mut = make_ctx(*ctx, sctx, "create_api_token");
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
      const auto sctx =
          middleware_.authenticate(extract_bearer(*ctx), rpc::CredentialRule::TokenAndMfa);

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
      const auto sctx =
          middleware_.authenticate(extract_bearer(*ctx), rpc::CredentialRule::TokenAndMfa);
      const auto api_token_id = core::ApiTokenId::parse(req->api_token_id());
      auth_.revoke_api_token(api_token_id, make_ctx(*ctx, sctx, "revoke_api_token"));
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

} // namespace fmgr::server
