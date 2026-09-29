// SPDX-License-Identifier: AGPL-3.0-or-later

#include "rpc/AuthMiddleware.h"

#include "auth/AuthTypes.h"
#include "core/permissions.h"

#include <atomic>
#include <cstddef>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
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

    std::string permission_key(core::Permission perm) {
      return std::string(core::to_key(perm));
    }

    // #60: the registry is a contract, not documentation. A handler that asks the
    // gate for a permission other than the one its RPC registered is a code
    // defect — relaxing a mutating RPC's registration is how #54 nearly shipped a
    // wrong permission with every test green — so the call is refused instead of
    // being served behind a gate nobody can look up.
    void require_registry_agreement(const RpcCall& call, core::Permission enforced) {
      if (call.method.empty()) {
        // Not inside a served RPC (unit tests, tooling): there is no registration
        // this call could contradict.
        return;
      }
      const auto registry = AuthMiddleware::registered_rpcs();
      const auto registered = registry.find(call.method);
      if (registered == registry.end()) {
        throw RpcRegistryMismatch("RPC " + call.method +
                                  " calls authorize() but is not in the permission registry; "
                                  "register it in its service constructor (#60)");
      }
      if (registered->second != enforced) {
        throw RpcRegistryMismatch("RPC " + call.method + " is registered as permission '" +
                                  permission_key(registered->second) +
                                  "' but its handler enforces '" + permission_key(enforced) +
                                  "'; the registration and the authorize() call must agree (#60)");
      }
    }

  } // namespace

  AuthMiddleware::AuthMiddleware(auth::IAuthProvider& auth) : auth_(auth) {}

  void AuthMiddleware::set_process_data_rate_limiter(rpc::RateLimiter* limiter) {
    process_data_limiter().store(limiter, std::memory_order_release);
  }

  auth::SessionContext AuthMiddleware::authorize(const RpcCall& call,
                                                 core::Permission required_perm,
                                                 std::optional<core::LabId> lab_id) const {
    // Step 0: the RPC's registration must agree with the permission this handler
    // is asking for (#60). Checked before any work — a disagreement is a code
    // defect, not something the caller can fix by presenting different
    // credentials.
    require_registry_agreement(call, required_perm);

    const std::string_view bearer_token = call.bearer_token;

    // Step 1: data-tier rate limit, keyed by the bearer token, before any work
    // (notably token validation). Throttles authenticated request floods across
    // every service (audit C-10). Skipped when no gate is installed.
    if (auto* limiter = process_data_limiter().load(std::memory_order_acquire)) {
      if (!limiter->try_acquire(std::string(bearer_token), rpc::RateLimiter::Clock::now())) {
        throw auth::RateLimited("too many requests; slow down");
      }
    }

    // Step 2: validate token (may throw InvalidCredentials, TokenExpired, etc.)
    auth::SessionContext ctx = auth_.validate_token(bearer_token);

    // Step 3: MFA gate
    if (!ctx.mfa_complete) {
      throw auth::MfaRequired("MFA verification required before accessing this operation");
    }

    // Step 4: scoped permission gate
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

  std::unordered_map<std::string, core::Permission> AuthMiddleware::registered_rpcs() {
    auto& reg = get_registry();
    std::scoped_lock lock(reg.mutex);
    return reg.map;
  }

  void AuthMiddleware::verify_registry_covers(std::span<const std::string> served_rpc_names) {
    const auto registry = registered_rpcs();
    std::string missing;
    std::size_t missing_count = 0;
    for (const auto& name : served_rpc_names) {
      if (registry.contains(name)) {
        continue;
      }
      ++missing_count;
      if (!missing.empty()) {
        missing += ", ";
      }
      missing += name;
    }
    if (missing_count > 0) {
      throw RpcRegistryMismatch(
          "the RPC registry is missing " + std::to_string(missing_count) +
          " served RPC(s): " + missing +
          "; every served RPC must be registered in its service constructor so its permission can "
          "be checked at the gate (#60)");
    }
  }

} // namespace fmgr::rpc
