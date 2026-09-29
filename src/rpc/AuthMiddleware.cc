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
      std::unordered_map<std::string, RpcGate> map;
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

  } // namespace

  std::string_view to_key(CredentialRule rule) {
    switch (rule) {
    case CredentialRule::None:
      return "no_credential";
    case CredentialRule::TokenOnly:
      return "token_only";
    case CredentialRule::TokenAndMfa:
      return "token_and_mfa";
    }
    throw std::logic_error("unknown credential rule (#119)");
  }

  core::Permission RpcGate::permission() const {
    if (!permission_.has_value()) {
      throw std::logic_error("this RPC is registered as " + describe() +
                             "; it has no permission to read (see rpc::RpcGate, #78)");
    }
    return *permission_;
  }

  CredentialRule RpcGate::credential_rule() const {
    if (!credential_.has_value()) {
      throw std::logic_error("this RPC is registered as " + describe() +
                             "; it declares no credential rule to read (see rpc::RpcGate, #119)");
    }
    return *credential_;
  }

  std::string RpcGate::describe() const {
    if (permission_.has_value()) {
      return "permission '" + permission_key(*permission_) + "'";
    }
    return "credential rule '" + std::string(to_key(*credential_)) + "'";
  }

  // #60: the registry is a contract, not documentation. A handler that asks the
  // gate for a permission other than the one its RPC registered is a code defect —
  // relaxing a mutating RPC's registration is how #54 nearly shipped a wrong
  // permission with every test green — so the call is refused instead of being
  // served behind a gate nobody can look up.
  void AuthMiddleware::require_registry_agreement(const RpcCall& call, core::Permission enforced) {
    if (call.method.empty()) {
      // Not inside a served RPC (unit tests, tooling): there is no registration
      // this call could contradict.
      return;
    }
    // One locked lookup, not a snapshot copy: this runs on every authenticated
    // RPC.
    auto& reg = get_registry();
    std::scoped_lock lock(reg.mutex);
    const auto registered = reg.map.find(call.method);
    if (registered == reg.map.end()) {
      throw RpcRegistryMismatch("RPC " + call.method +
                                " calls authorize() but is not in the permission registry; "
                                "register it in its service constructor (#60)");
    }
    const RpcGate& gate = registered->second;
    // Short-circuits on the kind, so permission() is only read when there is one
    // to read (#78).
    if (gate.kind() != RpcGate::Kind::Permission || gate.permission() != enforced) {
      throw RpcRegistryMismatch("RPC " + call.method + " is registered as " + gate.describe() +
                                " but its handler enforces '" + permission_key(enforced) +
                                "'; the registration and the authorize() call must agree (#60)");
    }
  }

  // #119: the same contract for the nine RPCs that never reach authorize(). Their
  // credential rule used to be a claim in the registry that no code path observed
  // — a handler could start demanding MFA and every test stayed green — so the
  // gate now compares the rule a handler applies against the one its RPC
  // declares, and refuses the call when they disagree. The mirror of #60, one
  // kind over: a handler that asks for a *different* credential rule from the one
  // it registered is a code defect, not a caller who presented the wrong
  // credential.
  void AuthMiddleware::require_credential_agreement(const RpcCall& call, CredentialRule required) {
    if (call.method.empty()) {
      // Not inside a served RPC (unit tests, tooling): there is no registration
      // this call could contradict.
      return;
    }
    auto& reg = get_registry();
    std::scoped_lock lock(reg.mutex);
    const auto registered = reg.map.find(call.method);
    if (registered == reg.map.end()) {
      throw RpcRegistryMismatch("RPC " + call.method +
                                " calls the credential gate but is not in the permission registry; "
                                "register it in its service constructor (#119)");
    }
    const RpcGate& gate = registered->second;
    // Short-circuits on the kind, so credential_rule() is only read when there is
    // one to read. A Permission entry is a disagreement too: the RPC declares a
    // permission this handler never asks authorize() for, so that entry would go
    // unenforced — #78's hole with the sign flipped.
    if (gate.kind() != RpcGate::Kind::Credential || gate.credential_rule() != required) {
      throw RpcRegistryMismatch("RPC " + call.method + " is registered as " + gate.describe() +
                                " but its handler requires '" + std::string(to_key(required)) +
                                "'; the registration and the credential the handler applies must "
                                "agree (#119)");
    }
  }

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

  auth::SessionContext AuthMiddleware::authenticate(const RpcCall& call,
                                                    CredentialRule rule) const {
    if (rule == CredentialRule::None) {
      // A rule that requires no credential has no session to hand back, so it is
      // declared with admit_no_credential() instead. Refusing here rather than
      // inventing an empty SessionContext keeps "no credential" from looking like
      // "a credential that resolved to nobody".
      throw std::logic_error("CredentialRule::None has no session context; declare it with "
                             "AuthMiddleware::admit_no_credential() (#119)");
    }

    // Step 0: the RPC's registration must declare the rule this handler applies
    // (#119). Same position as authorize()'s step 0, and for the same reason: a
    // disagreement is a code defect, not something the caller can fix by
    // presenting different credentials.
    require_credential_agreement(call, rule);

    // Step 1: validate token (may throw InvalidCredentials, TokenExpired, etc.)
    auth::SessionContext ctx = auth_.validate_token(call.bearer_token);

    // Step 2: the MFA half of the rule. TokenOnly skips it deliberately — that is
    // the whole of the #62 exception, and the reason the rule is named rather
    // than assumed. The message is the one these handlers used before #119 moved
    // the check here, so the wire text a client sees is unchanged.
    if (rule == CredentialRule::TokenAndMfa && !ctx.mfa_complete) {
      throw auth::MfaRequired("MFA required before this operation");
    }

    return ctx;
  }

  void AuthMiddleware::admit_no_credential(const RpcCall& call) const {
    // The declaration is the whole of the enforcement here: there is no credential
    // to validate, so what the gate can refuse is a registration that claims one.
    // call.bearer_token is deliberately unread — a caller of Login need not have a
    // credential, and must not be refused for the Authorization header it did or
    // did not send.
    require_credential_agreement(call, CredentialRule::None);
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

  void AuthMiddleware::register_rpc(std::string rpc_name, RpcGate gate) {
    auto& reg = get_registry();
    std::scoped_lock lock(reg.mutex);
    reg.map.insert_or_assign(std::move(rpc_name), gate);
  }

  std::unordered_map<std::string, RpcGate> AuthMiddleware::registered_rpcs() {
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
