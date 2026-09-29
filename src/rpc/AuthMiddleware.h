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

  // The credential a caller must present to an RPC that is not permission-gated
  // (#119).
  //
  // Three rules, not one. The registry's earlier second state could only say
  // "requires no permission": accurate, and unenforced, because the handlers that
  // use it never call authorize(). Collapsing the three into a single
  // "authenticated + MFA" state would have been false for three of the nine RPCs
  // outside the permission gate. Naming each rule is what lets the gate *apply*
  // it instead of recording it:
  //
  //   None         the handler admits a caller who has no credential at all.
  //                Login is the only one — it is where a caller gets one.
  //   TokenOnly    a valid session or API token, deliberately without MFA.
  //                Completing the second factor and giving the credential up
  //                must both work while it is still outstanding (#62), so
  //                SubmitMfa and Logout are the exact and entire exception.
  //   TokenAndMfa  a valid token whose second factor is complete.
  enum class CredentialRule {
    None,
    TokenOnly,
    TokenAndMfa,
  };

  // Stable name of a rule for messages: "no credential", "a token without MFA",
  // "a token with MFA". A refusal quotes the rule it names, so the string a test
  // asserts on is the one the registration describes itself with.
  [[nodiscard]] std::string_view to_key(CredentialRule rule);

  // What admits a call to an RPC — the registry's value type (#78, #119).
  //
  // The registry used to map an RPC straight to a `core::Permission`, so it had
  // no way to say "this RPC is gated by something other than a permission", and
  // ten entries named a permission their handler never checked: `ListSessions`
  // registered `session.revoke` while filtering to the caller's own rows,
  // `ListLabs` registered `lab.configure` while consulting `lab.provision` for
  // *visibility*, `VerifyAuditChain` registered `audit.read` while requiring
  // `is_system_admin`. Each kind below states a claim the gate can act on, and
  // no more than that:
  //
  //   Permission  the handler's authorize() call must name exactly this
  //               permission. The gate checks it on every call and refuses the
  //               call when it disagrees (#60), so this entry is verified, not
  //               documented.
  //   Credential  the handler must not be permission-gated at all, and this is
  //               the credential rule it applies. It asks the gate for the same
  //               rule — authenticate(), or admit_no_credential() for `None` —
  //               the gate validates the credential itself, and it refuses the
  //               call when the handler asks for a different rule (#119). An RPC
  //               registered this way whose handler calls authorize() is refused
  //               too, so the state cannot be used to silence the #60 check.
  class RpcGate {
  public:
    enum class Kind {
      Permission,
      Credential,
    };

    // Implicit on purpose: the permission-gated registrations keep reading as
    // `register_rpc(name, P::SampleRead)`.
    RpcGate(core::Permission permission) : kind_(Kind::Permission), permission_(permission) {}

    // The credential rules, as registrations: `register_rpc(name,
    // RpcGate::token_only())`. Named factories rather than an implicit
    // CredentialRule constructor, so a bare enum value at a call site cannot be
    // misread as a permission.
    [[nodiscard]] static RpcGate no_credential() {
      return RpcGate(CredentialRule::None);
    }
    [[nodiscard]] static RpcGate token_only() {
      return RpcGate(CredentialRule::TokenOnly);
    }
    [[nodiscard]] static RpcGate token_and_mfa() {
      return RpcGate(CredentialRule::TokenAndMfa);
    }
    // The same thing for code that holds a rule as a value — a test comparing a
    // declaration with the rule a handler applies.
    [[nodiscard]] static RpcGate credential(CredentialRule rule) {
      return RpcGate(rule);
    }

    [[nodiscard]] Kind kind() const {
      return kind_;
    }

    // Precondition: kind() == Kind::Permission. Throws std::logic_error
    // otherwise, so a gate with no permission cannot be read as if it had one.
    [[nodiscard]] core::Permission permission() const;

    // Precondition: kind() == Kind::Credential. Throws std::logic_error
    // otherwise, so a permission gate cannot be read as if it declared a
    // credential rule.
    [[nodiscard]] CredentialRule credential_rule() const;

    // Human-readable, for the refusal message: "permission 'sample.read'" or
    // "credential rule 'a token without MFA'".
    [[nodiscard]] std::string describe() const;

    friend bool operator==(const RpcGate&, const RpcGate&) = default;

  private:
    explicit RpcGate(CredentialRule rule) : kind_(Kind::Credential), credential_(rule) {}

    Kind kind_;
    std::optional<core::Permission> permission_;
    std::optional<CredentialRule> credential_;
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

    // Credential gate for the RPCs that are not permission-gated (#119).
    //
    // Those handlers never call authorize(), so before #119 the gate never
    // observed their registrations: their credential rule was declared in the
    // registry and applied by hand in each handler, and the two could diverge
    // silently. This is their gate. Steps:
    //   0. The RPC's registration must declare `rule` (see
    //      require_credential_agreement()). Checked before any credential work —
    //      a disagreement is a code defect, not something the caller can fix by
    //      presenting different credentials.
    //   1. Validates the bearer token via IAuthProvider::validate_token().
    //   2. Throws MfaRequired when rule == TokenAndMfa and ctx.mfa_complete is
    //      false. TokenOnly skips this step: that is the whole of the #62
    //      exception, and it is why the rule has to be named rather than assumed.
    //
    // No permission check — enforcing no permission is what the registration
    // says — and no data-tier rate limit: that is authorize()'s step for data
    // endpoints, and Login/SubmitMfa keep AuthServiceImpl's per-IP limiter.
    //
    // Precondition: rule != CredentialRule::None. There is no session context to
    // return for a rule that requires no credential; that rule is declared with
    // admit_no_credential().
    //
    // Throws: RpcRegistryMismatch (a code defect, → INTERNAL) or any AuthError
    //         subclass (InvalidCredentials, TokenExpired, MfaRequired, …)
    [[nodiscard]] auth::SessionContext authenticate(const RpcCall& call, CredentialRule rule) const;

    // The CredentialRule::None half of the credential gate: admits a call that
    // presents no credential at all, and refuses one whose RPC declares any other
    // rule. `call`'s bearer token is deliberately unread — a caller of Login need
    // not have one, and must not be refused for the Authorization header it did
    // or did not send. Returns nothing: there is no session to describe.
    //
    // Static, unlike authorize() and authenticate(): there is no credential to
    // validate, so this one needs nothing from the middleware instance. The
    // difference is the useful part — it is what says the other two are the ones
    // that talk to IAuthProvider.
    static void admit_no_credential(const RpcCall& call);

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
    // Each RPC handler file registers its RPC name + gate at startup. The
    // registry is not write-only metadata: authorize() checks the permission a
    // handler enforces against the entry its RPC registered, and refuses the call
    // when they disagree (#60); authenticate()/admit_no_credential() do the same
    // for the credential rule a non-permission handler applies (#119).
    // verify_registry_covers() does the other half at server startup — a served
    // RPC that is not registered cannot be looked up, so the server refuses to
    // start rather than serve it.
    //
    // An RPC that does not gate through authorize() (AGENTS.md §5) registers the
    // credential rule its handler applies — RpcGate::token_and_mfa(),
    // RpcGate::token_only() or RpcGate::no_credential() — rather than a
    // permission nothing checks. The gate applies that rule and refuses the call
    // when the handler asks it for a different one, and it still refuses an RPC
    // registered this way whose handler calls authorize().
    static void register_rpc(std::string rpc_name, RpcGate gate);
    // Returns a snapshot copy of the registry (safe for iteration in tests/CI).
    [[nodiscard]] static std::unordered_map<std::string, RpcGate> registered_rpcs();

    // Throws RpcRegistryMismatch naming every entry of `served_rpc_names` that is
    // missing from the registry. The server calls this from build(), so a new RPC
    // that ships without a registration fails at startup instead of being served
    // behind a gate the registry cannot describe. Extra registry entries are not
    // an error here: a binary may construct services it does not serve.
    static void verify_registry_covers(std::span<const std::string> served_rpc_names);

  private:
    // Step 0 of authorize(): throws RpcRegistryMismatch unless call's RPC is
    // registered with a Permission gate whose permission is `enforced` — any
    // Credential entry is a disagreement too, so registering an authorize()-gated
    // RPC that way refuses its calls rather than silencing the check (#78). No-op
    // when the call carries no RPC identity.
    static void require_registry_agreement(const RpcCall& call, core::Permission enforced);

    // Step 0 of authenticate()/admit_no_credential(): throws RpcRegistryMismatch
    // unless call's RPC is registered with a Credential gate whose rule is
    // `required` — a Permission entry is a disagreement too, because that entry
    // would otherwise never be enforced, which is #78's hole with the sign
    // flipped (#119). No-op when the call carries no RPC identity.
    static void require_credential_agreement(const RpcCall& call, CredentialRule required);

    auth::IAuthProvider& auth_;
  };

} // namespace fmgr::rpc

#endif // FMGR_RPC_AUTHMIDDLEWARE_H
