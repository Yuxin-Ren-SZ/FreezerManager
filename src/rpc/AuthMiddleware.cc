// SPDX-License-Identifier: AGPL-3.0-or-later

#include "rpc/AuthMiddleware.h"

#include "auth/AuthTypes.h"
#include "core/permissions.h"

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>

namespace fmgr::rpc {

  namespace {

    struct RpcRegistry {
      std::mutex mutex;
      std::unordered_map<std::string, core::Permission> map;
    };

    RpcRegistry& get_registry() {
      static RpcRegistry s_registry;
      return s_registry;
    }

    // Process-wide data-tier gate. Null unless a server installs one. Atomic so
    // install/uninstall races with in-flight authorize() calls are well-defined.
    std::atomic<RateLimiter*>& process_data_limiter() {
      static std::atomic<RateLimiter*> s_limiter{nullptr};
      return s_limiter;
    }

  } // namespace

  AuthMiddleware::AuthMiddleware(auth::IAuthProvider& auth) : auth_(auth) {}

  void AuthMiddleware::set_process_data_rate_limiter(rpc::RateLimiter* limiter) {
    process_data_limiter().store(limiter, std::memory_order_release);
  }

  auth::SessionContext AuthMiddleware::authorize(std::string_view bearer_token,
                                                 core::Permission required_perm,
                                                 std::optional<core::LabId> lab_id) const {
    // Step 0: data-tier rate limit, keyed by the bearer token, before any work
    // (notably token validation). Throttles authenticated request floods across
    // every service (audit C-10). Skipped when no gate is installed.
    if (auto* limiter = process_data_limiter().load(std::memory_order_acquire)) {
      if (!limiter->try_acquire(std::string(bearer_token), rpc::RateLimiter::Clock::now())) {
        throw auth::RateLimited("too many requests; slow down");
      }
    }

    // Step 1: validate token (may throw InvalidCredentials, TokenExpired, etc.)
    auth::SessionContext ctx = auth_.validate_token(bearer_token);

    // Step 2: MFA gate
    if (!ctx.mfa_complete) {
      throw auth::MfaRequired("MFA verification required before accessing this operation");
    }

    // Step 3: scoped permission gate
    if (lab_id.has_value()) {
      if (!ctx.has_for_lab(*lab_id, required_perm)) {
        throw auth::PermissionDenied("caller lacks required permission for target lab");
      }
    } else if (!ctx.has_global(required_perm)) {
      throw auth::PermissionDenied("caller lacks required deployment-wide permission");
    }

    return ctx;
  }

  void AuthMiddleware::inject_rls_vars(storage::ITransaction& txn,
                                       const auth::SessionContext& ctx) {
    // Pass bare keys — PostgresTransaction::set_session_var prepends "app." automatically.
    // SQLite no-op override is unaffected.
    txn.set_session_var("current_user_id", ctx.user_id.to_string());

    std::string lab_ids;
    for (const auto& [lab, permissions] : ctx.permissions_by_lab) {
      (void)permissions;
      if (!lab_ids.empty()) {
        lab_ids += ',';
      }
      lab_ids += lab.to_string();
    }
    txn.set_session_var("current_lab_ids", lab_ids);
  }

  void AuthMiddleware::register_rpc(std::string rpc_name, core::Permission required_perm) {
    auto& reg = get_registry();
    std::scoped_lock lock(reg.mutex);
    reg.map.insert_or_assign(std::move(rpc_name), required_perm);
  }

  bool AuthMiddleware::is_rpc_registered(std::string_view rpc_name) {
    auto& reg = get_registry();
    std::scoped_lock lock(reg.mutex);
    return reg.map.contains(std::string(rpc_name));
  }

  std::unordered_map<std::string, core::Permission> AuthMiddleware::registered_rpcs() {
    auto& reg = get_registry();
    std::scoped_lock lock(reg.mutex);
    return reg.map;
  }

} // namespace fmgr::rpc
