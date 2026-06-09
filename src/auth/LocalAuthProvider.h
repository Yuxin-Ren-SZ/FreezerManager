// SPDX-License-Identifier: AGPL-3.0-or-later

// E2: Password + TOTP authentication provider backed by the local SQLite/Postgres DB.
//
// authenticate(PasswordCredentials) flow:
//   1. Lower-case email, look up active User row.
//   2. Find {"provider":"local","hash":"..."} in User.auth_bindings.
//   3. Verify against stored Argon2id hash (crypto_pwhash_str_verify).
//   4. Apply account-lockout policy (in-memory, resets on restart).
//   5. If user has totp_secret_enc set, create session with mfa_complete=false;
//      caller must invoke verify_totp() before the session gains full access.
//   6. Generate a 32-byte random session token; store BLAKE2b hash in sessions table.
//   7. Return plaintext token in AuthToken (shown once, never stored).
//
// authenticate(ApiTokenCredentials) flow:
//   Looks up ApiToken by prefix, BLAKE2b-verifies the submitted token, checks
//   expiry, builds SessionContext, and returns a synthetic AuthToken without
//   creating a Session row. API tokens bypass TOTP (they are already a second
//   factor).
//
// validate_token(bearer) dispatches on the "fmgr_pat_" prefix to session vs.
// API-token path; both paths resolve to a full SessionContext.
//
// Thread-safety: all public methods are safe to call concurrently; the
// in-memory lockout map is protected by a std::mutex.
#ifndef FMGR_AUTH_LOCALAUTHPROVIDER_H
#define FMGR_AUTH_LOCALAUTHPROVIDER_H

#include "auth/AuthTypes.h"
#include "auth/IAuthProvider.h"
#include "auth/Totp.h"
#include "core/session.h"
#include "storage/IStorageBackend.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>

namespace fmgr::auth {

  struct LocalAuthProviderConfig {
    // Argon2id parameters for password hashing.
    // In tests, use memlimit = crypto_pwhash_MEMLIMIT_MIN (8192) and opslimit = 1
    // to avoid multi-second delays.
    std::size_t pwhash_memlimit{64ULL * 1024 * 1024}; // 64 MiB default (OWASP)
    std::uint64_t pwhash_opslimit{3};

    // Number of leading hex characters stored as the indexed lookup prefix.
    std::size_t token_prefix_len{16};

    // Account lockout: lock after this many consecutive failures.
    int max_failures_before_lockout{5};
    // Duration of the lockout window in seconds.
    std::int64_t lockout_duration_seconds{3600}; // 1 hour

    // TOTP window / step parameters.
    TotpConfig totp{};

    // D9.3: session expiry limits (seconds).
    std::int64_t max_session_idle_seconds{12LL * 3600};    // 12 h idle
    std::int64_t max_session_abs_seconds{7LL * 24 * 3600}; // 7 d absolute

    // Rate-limit last_seen_at updates: skip if updated within this interval.
    std::int64_t last_seen_update_interval_seconds{60};

    // Permission-context cache TTL (seconds). 0 disables caching.
    std::int64_t session_ctx_cache_ttl_seconds{300}; // 5 min default
  };

  class LocalAuthProvider final : public IAuthProvider {
  public:
    explicit LocalAuthProvider(storage::IStorageBackend& backend,
                               LocalAuthProviderConfig config = {});

    ~LocalAuthProvider() override = default;

    LocalAuthProvider(const LocalAuthProvider&) = delete;
    LocalAuthProvider& operator=(const LocalAuthProvider&) = delete;

    // Produce an Argon2id hash string suitable for storage in User.auth_bindings.
    // Also used by first-run wizards and test fixtures to create local users.
    [[nodiscard]] std::string hash_password(std::string_view plaintext) const;

    // ---- IAuthProvider ----

    AuthToken authenticate(const AuthCredentials& creds, const ClientInfo& client) override;
    SessionContext validate_token(std::string_view bearer_token) override;
    void verify_totp(const core::SessionId& session_id, std::string_view totp_code) override;
    void revoke_session(const core::SessionId& session_id,
                        const storage::MutationContext& ctx) override;
    void revoke_all_sessions(const core::UserId& uid, const storage::MutationContext& ctx) override;

  private:
    storage::IStorageBackend& backend_;
    LocalAuthProviderConfig config_;

    struct LockoutState {
      int failure_count{0};
      std::optional<core::Timestamp> locked_until;
    };

    mutable std::mutex lockout_mutex_;
    std::unordered_map<std::string, LockoutState> lockout_map_;

    // Permission-context cache (D9.3 / E3). Caches the resolve_permissions result
    // keyed by session-id string. MFA flag always comes from the DB session row,
    // not from this cache, so verify_totp() takes effect on the next request.
    struct CachedContext {
      core::UserId user_id;
      std::map<core::LabId, std::set<core::Permission>> permissions_by_lab;
      std::set<core::Permission> global_permissions;
      std::chrono::steady_clock::time_point cached_at;
      // User.authz_version captured when this entry was built. A later request
      // whose freshly-read authz_version differs invalidates the entry even
      // within the TTL, so permission downgrades take effect immediately.
      std::int64_t authz_version{0};
    };

    mutable std::mutex cache_mutex_;
    mutable std::unordered_map<std::string, CachedContext> ctx_cache_;

    void cache_evict(const std::string& session_id_str);
    void cache_evict_user(const core::UserId& uid);

    // D9.3 session expiry helpers (called from validate_session_token).
    void check_session_expiry(const core::Session& session, core::Timestamp now) const;
    void update_last_seen_if_needed(const core::Session& session, core::Timestamp now);
    [[nodiscard]] SessionContext lookup_or_build_context(const core::Session& session,
                                                         std::int64_t authz_version);

    // ---- internal helpers ----

    AuthToken do_password_auth(const PasswordCredentials& creds, const ClientInfo& client);
    AuthToken do_api_token_auth(const ApiTokenCredentials& creds, const ClientInfo& client);

    SessionContext validate_session_token(std::string_view bearer_token);
    SessionContext validate_api_token(std::string_view bearer_token);

    [[nodiscard]] SessionContext build_session_context(const core::Session& session) const;
    [[nodiscard]] SessionContext build_api_token_context(const core::ApiToken& token) const;

    // Returns true if the user's totp_secret_enc is populated (TOTP enrolled).
    [[nodiscard]] bool user_has_totp(const core::UserId& uid) const;

    // Token generation and hashing.
    [[nodiscard]] std::string generate_token() const;                   // 64 hex chars
    [[nodiscard]] std::string hash_token(std::string_view token) const; // BLAKE2b-256 hex
    [[nodiscard]] bool verify_token_hash(std::string_view token,
                                         std::string_view stored_hash) const;
    [[nodiscard]] std::string prefix_of(std::string_view token) const;

    // UUID v4 generation for new Session IDs.
    [[nodiscard]] static core::SessionId make_session_id();

    // Convenience MutationContext for system-initiated auth mutations.
    [[nodiscard]] storage::MutationContext system_ctx(std::string_view reason) const;

    // Account-lockout helpers (all require caller to NOT hold lockout_mutex_).
    void record_failure(const std::string& lower_email);
    void record_success(const std::string& lower_email);
    [[nodiscard]] std::optional<core::Timestamp>
    check_lockout(const std::string& lower_email) const;

    // Current time as a Timestamp.
    [[nodiscard]] static core::Timestamp now_ts();
  };

} // namespace fmgr::auth

#endif // FMGR_AUTH_LOCALAUTHPROVIDER_H
