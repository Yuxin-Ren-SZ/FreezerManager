// SPDX-License-Identifier: AGPL-3.0-or-later

#include "server/SessionServiceImpl.h"
#include "server/RequestId.h"

#include "core/permissions.h"
#include "core/session.h"
#include "server/GrpcErrorTranslation.h"
#include "storage/IStorageBackend.h"
#include "storage/SessionTraits.h"

#include <fmgr/v1/session.grpc.pb.h>
#include <grpcpp/grpcpp.h>

namespace fmgr::server {
  namespace {

    [[nodiscard]] auth::SessionContext validate_authed(auth::IAuthProvider& auth,
                                                       const grpc::ServerContext& ctx) {
      const auto bearer = extract_bearer(ctx);
      auto sctx = auth.validate_token(bearer);
      if (!sctx.mfa_complete) {
        throw auth::MfaRequired("MFA required before this operation");
      }
      return sctx;
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

    void fill_session_summary(fmgr::v1::SessionSummary* out, const core::Session& s) {
      out->set_id(s.id.to_string());
      out->set_user_id(s.user_id.to_string());
      out->set_token_prefix(s.token_prefix);
      out->mutable_created_at()->set_unix_micros(s.created_at.unix_micros());
      out->mutable_last_seen_at()->set_unix_micros(s.last_seen_at.unix_micros());
      if (s.ip.has_value()) {
        out->set_ip(*s.ip);
      }
      if (s.user_agent.has_value()) {
        out->set_user_agent(*s.user_agent);
      }
      if (s.revoked_at.has_value()) {
        out->mutable_revoked_at()->set_unix_micros(s.revoked_at->unix_micros());
      }
      out->set_mfa_complete(s.mfa_complete);
    }

  } // namespace

  SessionServiceImpl::SessionServiceImpl(auth::IAuthProvider& auth,
                                         storage::IStorageBackend& backend)
      : auth_(auth), backend_(backend), middleware_(auth) {
    // Both act on the caller's own sessions (ListSessions filters by the caller's
    // user id; RevokeSession revokes by id for the authenticated caller): a
    // valid, MFA-complete bearer is required, but no specific permission.
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.SessionService/ListSessions",
                                      rpc::RpcPolicy::self_service());
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.SessionService/RevokeSession",
                                      rpc::RpcPolicy::self_service());
  }

  grpc::Status SessionServiceImpl::ListSessions(grpc::ServerContext* ctx,
                                                const fmgr::v1::ListSessionsRequest* req,
                                                fmgr::v1::ListSessionsResponse* resp) {
    try {
      const auto sctx = validate_authed(auth_, *ctx);

      auto query = storage::Query<core::Session>::where(
          storage::field<core::Session, std::string>(core::Session::Field::UserId) ==
          sctx.user_id.to_string());
      if (req->include_revoked()) {
        query = query.include_tombstoned();
      }

      auto txn = backend_.begin(storage::IsolationLevel::ReadCommitted);
      rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);
      const auto sessions = txn->repo<core::Session>().query(query);
      txn->commit();

      for (const auto& s : sessions) {
        fill_session_summary(resp->add_sessions(), s);
      }
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

  grpc::Status SessionServiceImpl::RevokeSession(grpc::ServerContext* ctx,
                                                 const fmgr::v1::RevokeSessionRequest* req,
                                                 fmgr::v1::RevokeSessionResponse* /*resp*/) {
    try {
      const auto sctx = validate_authed(auth_, *ctx);
      const auto session_id = core::SessionId::parse(req->session_id());
      auth_.revoke_session(session_id, make_ctx(*ctx, sctx, "revoke_session"));
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

} // namespace fmgr::server
