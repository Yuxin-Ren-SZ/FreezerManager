// SPDX-License-Identifier: AGPL-3.0-or-later

// E3: RBAC gate that sits between a raw bearer token and every RPC handler.
//
// Usage inside a gRPC/REST handler:
//   auto ctx = middleware_.authorize(bearer, Permission::SampleRead, lab_id);
//   auto tx  = backend_.begin(IsolationLevel::Serializable);
//   AuthMiddleware::inject_rls_vars(*tx, ctx);
//   // ... use tx->repo<...>() ...
//   tx->commit();
//
// authorize() guarantee:
//   On success: the returned SessionContext is fully populated; the caller
//   holds the required permission in the requested scope; mfa_complete == true.
//   On failure: an AuthError subclass is thrown and the handler must not
//   proceed. The specific subtype tells the caller what to surface to the
//   client (InvalidCredentials → 401, PermissionDenied → 403, etc.).
#ifndef FMGR_RPC_AUTHMIDDLEWARE_H
#define FMGR_RPC_AUTHMIDDLEWARE_H

#include "auth/AuthTypes.h"
#include "auth/IAuthProvider.h"
#include "core/ids.h"
#include "core/permissions.h"
#include "rpc/RateLimiter.h"
#include "storage/IStorageBackend.h"

#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>

namespace fmgr::rpc {

  // The RPC a call is being served for, alongside the caller's bearer token.
  //
  // `method` is the gRPC full method name ("/fmgr.v1.SampleService/ListSamples"),
  // which is the key the RPC → permission registry is written in. Carrying it
  // with the token is what lets authorize() check itself against the registry
  // without every handler passing an extra argument: `server::extract_bearer(ctx)`
  // returns both, and the handler already calls it at its gate.
  //
  // An empty method means "not inside a served RPC" (unit tests, tooling); the
  // registry check is skipped — there is no RPC whose registration could
  // disagree. On the served path the server installs the interceptor that fills
  // it in (see rpc/RpcMethodTracker.h).
  struct RpcCall {
    std::string bearer_token;
    std::string method;

    // The raw token, so callers that only need the credential (validate_token,
    // make_ctx, …) keep working unchanged.
    [[nodiscard]] operator std::string_view() const {
      return bearer_token;
    }
  };

  // Thrown when the permission a handler enforces is not the permission its RPC
  // registered — a code defect (most dangerous when a *mutating* RPC's
  // registration is relaxed, #54), never the caller's fault. Translates to
  // INTERNAL, and on the served path the call is refused rather than allowed
  // through a gate the registry does not describe (#60).
  class RpcRegistryMismatch : public std::logic_error {
  public:
    using std::logic_error::logic_error;
  };

  class AuthMiddleware {
  public:
    explicit AuthMiddleware(auth::IAuthProvider& auth);

    // Primary gate — call at the top of every RPC handler.
    //
    // Steps (in order, short-circuits on first failure):
    //   0. Verifies that `call` carries the RPC whose handler is asking, and that
    //      its registered permission is `required_perm` (see
    //      require_registry_agreement()). Skipped only when the call carries no
    //      RPC identity at all.
    //   1. Validates bearer token via IAuthProvider::validate_token().
    //   2. Throws MfaRequired if ctx.mfa_complete == false.
    //   3. If lab_id is set, throws PermissionDenied unless the caller holds
    //      required_perm for that lab.
    //   4. If lab_id is unset, throws PermissionDenied unless the caller holds
    //      required_perm as a deployment-wide permission.
    //
    // Throws: RpcRegistryMismatch (a code defect, → INTERNAL) or any AuthError
    //         subclass (InvalidCredentials, TokenExpired, MfaRequired,
    //         PermissionDenied, …)
    [[nodiscard]] auth::SessionContext
    authorize(const RpcCall& call, core::Permission required_perm,
              std::optional<core::LabId> lab_id = std::nullopt) const;

    // ---- Global data-tier rate limiting ----
    //
    // authorize() is the single choke point every authenticated RPC handler
    // passes through, so throttling here throttles all data endpoints across
    // every service without per-handler code (security audit C-10/DoS). The
    // limiter is a process-wide gate installed by the server for its lifetime;
    // when unset (the default, e.g. in unit tests), authorize() does not rate
    // limit. Health/metrics endpoints never reach authorize(), so they are
    // exempt by construction. Auth endpoints (Login/SubmitMfa) are throttled
    // separately at a higher burst by AuthServiceImpl's per-IP limiter.
    //
    // Installs `limiter` (may be null to uninstall) as the process gate. The
    // caller owns the limiter and must uninstall (pass nullptr) before it dies.
    static void set_process_data_rate_limiter(rpc::RateLimiter* limiter);

    // Inject Postgres RLS session variables into a transaction.
    // Sets "app.current_user_id" and "app.current_lab_ids" (comma-joined).
    // No-op for SQLite (ITransaction::set_session_var defaults to no-op).
    // Must be called after authorize() and before any repo operations.
    static void inject_rls_vars(storage::ITransaction& txn, const auth::SessionContext& ctx);

    // ---- RPC permission registry ----
    //
    // Each RPC handler file registers its RPC name + required permission at
    // startup. The registry is not write-only metadata: authorize() checks the
    // permission a handler enforces against the entry its RPC registered, and
    // refuses the call when they disagree (#60). verify_registry_covers() does
    // the other half at server startup — a served RPC that is not registered
    // cannot be looked up, so the server refuses to start rather than serve it.
    static void register_rpc(std::string rpc_name, core::Permission required_perm);
    // Returns a snapshot copy of the registry (safe for iteration in tests/CI).
    [[nodiscard]] static std::unordered_map<std::string, core::Permission> registered_rpcs();

    // Throws RpcRegistryMismatch naming every entry of `served_rpc_names` that is
    // missing from the registry. The server calls this from build(), so a new RPC
    // that ships without a registration fails at startup instead of being served
    // behind a gate the registry cannot describe. Extra registry entries are not
    // an error here: a binary may construct services it does not serve.
    static void verify_registry_covers(std::span<const std::string> served_rpc_names);

  private:
    // Step 0 of authorize(): throws RpcRegistryMismatch unless `enforced` is the
    // permission registered for call's RPC. No-op when the call carries no RPC
    // identity.
    static void require_registry_agreement(const RpcCall& call, core::Permission enforced);

    auth::IAuthProvider& auth_;
  };

} // namespace fmgr::rpc

#endif // FMGR_RPC_AUTHMIDDLEWARE_H
