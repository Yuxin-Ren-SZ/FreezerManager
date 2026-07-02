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
#include <string>
#include <string_view>
#include <unordered_map>

namespace fmgr::rpc {

  class AuthMiddleware {
  public:
    explicit AuthMiddleware(auth::IAuthProvider& auth);

    // Primary gate — call at the top of every RPC handler.
    //
    // Steps (in order, short-circuits on first failure):
    //   1. Validates bearer token via IAuthProvider::validate_token().
    //   2. Throws MfaRequired if ctx.mfa_complete == false.
    //   3. If lab_id is set, throws PermissionDenied unless the caller holds
    //      required_perm for that lab.
    //   4. If lab_id is unset, throws PermissionDenied unless the caller holds
    //      required_perm as a deployment-wide permission.
    //
    // Throws: any AuthError subclass (InvalidCredentials, TokenExpired,
    //         MfaRequired, PermissionDenied, …)
    [[nodiscard]] auth::SessionContext
    authorize(std::string_view bearer_token, core::Permission required_perm,
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
    // startup.  A CI test (added in F2) asserts that every known gRPC method
    // name appears in this registry — ensuring nothing bypasses the gate.
    static void register_rpc(std::string rpc_name, core::Permission required_perm);
    [[nodiscard]] static bool is_rpc_registered(std::string_view rpc_name);
    // Returns a snapshot copy of the registry (safe for iteration in tests/CI).
    [[nodiscard]] static std::unordered_map<std::string, core::Permission> registered_rpcs();

  private:
    auth::IAuthProvider& auth_;
  };

} // namespace fmgr::rpc

#endif // FMGR_RPC_AUTHMIDDLEWARE_H
