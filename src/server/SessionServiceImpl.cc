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

    // #77: does `session_id` belong to the caller? find_by_id() returns
    // tombstoned rows as well, which keeps a repeated revoke of one's own
    // session the idempotent no-op the IAuthProvider contract promises instead
    // of turning it into a permission error. A session's owner never changes,
    // so checking it in a separate read cannot race the tombstone write.
    //
    // The read is unfiltered only because `sessions` carries no RLS policy
    // (Postgres enables RLS on lab-scoped tables alone) and inject_rls_vars() is
    // a no-op on SQLite. Scoping sessions to a lab in a future migration would
    // change what this call can see, and with it the self-logout path.
    [[nodiscard]] bool caller_owns_session(storage::IStorageBackend& backend,
                                           const auth::SessionContext& sctx,
                                           const core::SessionId& session_id) {
      auto txn = backend.begin(storage::IsolationLevel::ReadCommitted);
      rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);
      const auto target = txn->repo<core::Session>().find_by_id(session_id);
      txn->commit();
      return target.has_value() && target->user_id == sctx.user_id;
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
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.SessionService/ListSessions",
                                      core::Permission::SessionRevoke);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.SessionService/RevokeSession",
                                      core::Permission::SessionRevoke);
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

      // #77: a caller may always revoke its own sessions -- that logout path is
      // why this RPC exists. Revoking someone else's is the SystemAdmin
      // exception the proto documents, and it is gated on the named
      // session.revoke permission rather than happening by accident.
      // session.revoke is global-only (core/permissions.h), so `has_global` is
      // the whole test: a lab-owned role can neither be granted it nor, if a
      // grant predates that classification, spend it. The denial is decided by
      // ownership, never by existence, so it is not an oracle for whether a
      // session id is live.
      if (!caller_owns_session(backend_, sctx, session_id) &&
          !sctx.has_global(core::Permission::SessionRevoke)) {
        throw auth::PermissionDenied("caller may not revoke another user's session");
      }

      auth_.revoke_session(session_id, make_ctx(*ctx, sctx, "revoke_session"));
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

} // namespace fmgr::server
