// SPDX-License-Identifier: AGPL-3.0-or-later

#include "auth/LocalAuthProvider.h"

#include "core/identity.h"
#include "core/role.h"
#include "core/session.h"
#include "core/uuid.h"
#include "storage/IStorageBackend.h"
#include "storage/IdentityTraits.h"
#include "storage/RoleTraits.h"
#include "storage/SessionTraits.h"

#include <nlohmann/json.hpp>
#include <sodium.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace fmgr::auth {
  namespace {

    // NOLINTNEXTLINE(modernize-avoid-c-arrays) — constexpr lookup table
    constexpr char k_hex_chars[] = "0123456789abcdef";

    [[nodiscard]] std::string bytes_to_hex(const unsigned char* data, std::size_t len) {
      std::string result;
      result.reserve(len * 2);
      for (std::size_t i = 0; i < len; ++i) {
        result += k_hex_chars[(data[i] >> 4U) & 0xFU];
        result += k_hex_chars[data[i] & 0xFU];
      }
      return result;
    }

    [[nodiscard]] core::Timestamp now_timestamp() {
      const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();
      return core::Timestamp::from_unix_micros(micros);
    }

    // Parses scope_json into an optional permission set.
    // Returns nullopt for the unrestricted sentinel ["*"].
    // Returns an empty set for malformed JSON or "[]" (fail-closed: zero permissions).
    [[nodiscard]] std::optional<std::set<core::Permission>>
    permissions_from_scope_json(const std::string& scope_json) {
      const auto parsed = nlohmann::json::parse(scope_json, nullptr, false);
      if (!parsed.is_array()) {
        return std::set<core::Permission>{}; // malformed → fail-closed
      }
      // ["*"] sentinel means unrestricted — inherit all user permissions.
      if (parsed.size() == 1 && parsed[0].is_string() && parsed[0].get<std::string>() == "*") {
        return std::nullopt;
      }
      std::set<core::Permission> perms;
      for (const auto& item : parsed) {
        if (item.is_string()) {
          try {
            perms.insert(core::parse_permission(item.get<std::string>()));
            // NOLINTNEXTLINE(bugprone-empty-catch)
          } catch (const std::exception&) {
            // Unknown permission keys are silently skipped.
          }
        }
      }
      return perms;
    }

    struct ScopedPermissionGrants {
      std::map<core::LabId, std::set<core::Permission>> permissions_by_lab;
      std::set<core::Permission> global_permissions;
    };

    // base = the user's role permissions, scope = the token's allowed subset; the
    // intersection is order-independent so swapping is harmless, but keep names clear.
    // NOLINTBEGIN(bugprone-easily-swappable-parameters)
    [[nodiscard]] std::set<core::Permission>
    intersect_permissions(const std::set<core::Permission>& base,
                          const std::set<core::Permission>& scope) {
      // NOLINTEND(bugprone-easily-swappable-parameters)
      std::set<core::Permission> result;
      for (const auto permission : base) {
        if (scope.contains(permission)) {
          result.insert(permission);
        }
      }
      return result;
    }

    [[nodiscard]] ScopedPermissionGrants
    intersect_grants_with_scope(ScopedPermissionGrants grants,
                                const std::optional<std::set<core::Permission>>& scope) {
      if (!scope.has_value()) {
        return grants; // unrestricted — inherit all
      }
      // scope is defined (possibly empty → zero permissions after intersect).
      // Prune labs whose permission set becomes empty so can_see_lab stays accurate.
      for (auto it = grants.permissions_by_lab.begin(); it != grants.permissions_by_lab.end();) {
        it->second = intersect_permissions(it->second, *scope);
        it = it->second.empty() ? grants.permissions_by_lab.erase(it) : std::next(it);
      }
      grants.global_permissions = intersect_permissions(grants.global_permissions, *scope);
      return grants;
    }

    // Resolve effective permissions for a user from their active lab memberships.
    [[nodiscard]] ScopedPermissionGrants resolve_permissions(storage::ITransaction& txn,
                                                             const core::UserId& user_id) {
      const auto memberships =
          txn.repo<core::LabMembership>().query(storage::Query<core::LabMembership>::where(
              storage::field<core::LabMembership, std::string>(
                  core::LabMembership::Field::UserId) == user_id.to_string()));

      ScopedPermissionGrants grants;

      for (const auto& membership : memberships) {
        if (membership.revoked_at.has_value()) {
          continue;
        }

        auto& lab_permissions = grants.permissions_by_lab[membership.lab_id];
        if (!membership.role_id.has_value()) {
          continue;
        }

        const auto role = txn.repo<core::Role>().find_by_id(*membership.role_id);
        if (!role.has_value() || role->archived_at.has_value()) {
          continue;
        }

        const auto role_perms =
            txn.repo<core::RolePermission>().query(storage::Query<core::RolePermission>::where(
                storage::field<core::RolePermission, std::string>(
                    core::RolePermission::Field::RoleId) == membership.role_id->to_string()));
        for (const auto& role_perm : role_perms) {
          if (core::is_global_only_permission(role_perm.permission)) {
            if (role->kind == core::RoleKind::SystemAdmin) {
              grants.global_permissions.insert(role_perm.permission);
            }
            continue;
          }
          lab_permissions.insert(role_perm.permission);
        }
      }

      return grants;
    }

  } // namespace

  // ---- Constructor ----

  LocalAuthProvider::LocalAuthProvider(storage::IStorageBackend& backend,
                                       LocalAuthProviderConfig config)
      : backend_(backend), config_(config) {
    if (sodium_init() < 0) {
      throw AuthError("libsodium failed to initialize");
    }
  }

  // ---- Public: hash_password ----

  std::string LocalAuthProvider::hash_password(std::string_view plaintext) const {
    std::array<char, crypto_pwhash_STRBYTES> hash{};
    if (crypto_pwhash_str(hash.data(), plaintext.data(), plaintext.size(), config_.pwhash_opslimit,
                          config_.pwhash_memlimit) != 0) {
      throw AuthError("argon2id: out of memory during password hashing");
    }
    return {hash.data()};
  }

  // ---- Public: authenticate ----

  AuthToken LocalAuthProvider::authenticate(const AuthCredentials& creds,
                                            const ClientInfo& client) {
    return std::visit(
        [&](const auto& credential) -> AuthToken {
          using CredT = std::decay_t<decltype(credential)>;
          if constexpr (std::is_same_v<CredT, PasswordCredentials>) {
            return do_password_auth(credential, client);
          } else {
            return do_api_token_auth(credential, client);
          }
        },
        creds);
  }

  // ---- Public: validate_token ----

  SessionContext LocalAuthProvider::validate_token(std::string_view bearer_token) {
    if (bearer_token.starts_with("fmgr_pat_")) {
      return validate_api_token(bearer_token);
    }
    return validate_session_token(bearer_token);
  }

  // ---- Public: verify_totp ----

  void LocalAuthProvider::verify_totp(const core::SessionId& session_id,
                                      std::string_view totp_code) {
    auto txn = backend_.begin(storage::IsolationLevel::Serializable);

    const auto maybe_session = txn->repo<core::Session>().find_by_id(session_id);
    if (!maybe_session.has_value() || maybe_session->revoked_at.has_value()) {
      txn->commit();
      throw InvalidCredentials("invalid session for TOTP verification");
    }
    if (maybe_session->mfa_complete) {
      txn->commit();
      throw InvalidCredentials("session MFA is already complete");
    }

    // Retrieve the user's TOTP secret.
    const auto users = txn->repo<core::User>().query(storage::Query<core::User>::where(
        storage::field<core::User, std::string>(core::User::Field::Id) ==
        maybe_session->user_id.to_string()));
    if (users.empty()) {
      txn->commit();
      throw InvalidCredentials("no TOTP secret configured for this user");
    }
    const auto& user = users.front();
    if (!user.totp_secret_enc.has_value()) {
      txn->commit();
      throw InvalidCredentials("no TOTP secret configured for this user");
    }

    const auto& totp_secret = *user.totp_secret_enc;
    const auto now_seconds = std::chrono::duration_cast<std::chrono::seconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();

    if (!totp_verify(totp_secret, totp_code, now_seconds, config_.totp)) {
      txn->commit();
      throw InvalidCredentials("invalid TOTP code");
    }

    auto updated_session = *maybe_session;
    updated_session.mfa_complete = true;
    const auto ctx = system_ctx("verify_totp");
    txn->repo<core::Session>().update(updated_session, ctx);
    txn->commit();
  }

  // ---- Public: revoke_session ----

  void LocalAuthProvider::revoke_session(const core::SessionId& session_id,
                                         const storage::MutationContext& ctx) {
    auto txn = backend_.begin(storage::IsolationLevel::Serializable);
    try {
      txn->repo<core::Session>().soft_delete(session_id, ctx);
      // NOLINTNEXTLINE(bugprone-empty-catch)
    } catch (const storage::NotFound&) {
      // Already revoked or never existed — idempotent per interface contract.
    }
    txn->commit();
    cache_evict(session_id.to_string());
  }

  // ---- Public: revoke_all_sessions ----

  void LocalAuthProvider::revoke_all_sessions(const core::UserId& uid,
                                              const storage::MutationContext& ctx) {
    auto txn = backend_.begin(storage::IsolationLevel::Serializable);
    const auto sessions = txn->repo<core::Session>().query(storage::Query<core::Session>::where(
        storage::field<core::Session, std::string>(core::Session::Field::UserId) ==
        uid.to_string()));
    for (const auto& session : sessions) {
      txn->repo<core::Session>().soft_delete(session.id, ctx);
    }
    txn->commit();
    cache_evict_user(uid);
  }

  // ---- Private: do_password_auth ----

  AuthToken LocalAuthProvider::do_password_auth(const PasswordCredentials& creds,
                                                const ClientInfo& client) {
    // Normalize email for lookup and lockout tracking.
    std::string lower_email = creds.email;
    std::ranges::transform(lower_email, lower_email.begin(),
                           [](unsigned char chr) { return static_cast<char>(::tolower(chr)); });

    // Lockout check before touching the DB (avoid timing oracle).
    if (auto locked = check_lockout(lower_email); locked.has_value()) {
      throw AccountLocked(*locked);
    }

    // Load the user.
    auto rtxn = backend_.begin(storage::IsolationLevel::ReadCommitted);
    const auto users = rtxn->repo<core::User>().query(storage::Query<core::User>::where(
        storage::field<core::User, std::string>(core::User::Field::PrimaryEmail) == lower_email));
    rtxn->commit();

    if (users.empty() || users.front().status != core::UserStatus::Active) {
      record_failure(lower_email);
      throw InvalidCredentials("invalid credentials");
    }
    const auto& user = users.front();

    // Find local auth binding in auth_bindings JSON array.
    std::string stored_hash;
    bool found_local = false;
    for (const auto& binding : user.auth_bindings) {
      if (binding.contains("provider") && binding.at("provider") == "local" &&
          binding.contains("hash")) {
        stored_hash = binding.at("hash").get<std::string>();
        found_local = true;
        break;
      }
    }
    if (!found_local) {
      record_failure(lower_email);
      throw InvalidCredentials("invalid credentials");
    }

    // Verify password (constant-time comparison via libsodium).
    if (crypto_pwhash_str_verify(stored_hash.c_str(), creds.password.data(),
                                 creds.password.size()) != 0) {
      record_failure(lower_email);
      // Surface AccountLocked immediately if this failure triggered the lockout.
      if (auto locked = check_lockout(lower_email); locked.has_value()) {
        throw AccountLocked(*locked);
      }
      throw InvalidCredentials("invalid credentials");
    }

    record_success(lower_email);

    // Determine MFA requirement.
    const bool mfa_complete = !user_has_totp(user.id);

    // Generate session token.
    const auto plaintext_token = generate_token();
    const auto token_prefix = prefix_of(plaintext_token);
    const auto token_hash = hash_token(plaintext_token);

    const auto now = now_ts();
    const core::Session session{
        .id = make_session_id(),
        .user_id = user.id,
        .token_hash = token_hash,
        .token_prefix = token_prefix,
        .created_at = now,
        .last_seen_at = now,
        .ip = client.ip,
        .user_agent = client.user_agent,
        .mfa_complete = mfa_complete,
    };

    auto wtxn = backend_.begin(storage::IsolationLevel::Serializable);
    const auto ctx = system_ctx("password_authenticate");
    wtxn->repo<core::Session>().insert(session, ctx);
    wtxn->commit();

    return AuthToken{
        .session_id = session.id,
        .plaintext_token = plaintext_token,
        .mfa_complete = mfa_complete,
    };
  }

  // ---- Private: do_api_token_auth ----

  AuthToken LocalAuthProvider::do_api_token_auth(const ApiTokenCredentials& creds,
                                                 const ClientInfo& /*client*/) {
    const std::string_view full = creds.token;
    if (!full.starts_with("fmgr_pat_") || full.size() < 9 + config_.token_prefix_len) {
      throw InvalidCredentials("malformed API token");
    }

    const auto token_prefix = prefix_of(full);

    auto rtxn = backend_.begin(storage::IsolationLevel::ReadCommitted);
    const auto tokens = rtxn->repo<core::ApiToken>().query(storage::Query<core::ApiToken>::where(
        storage::field<core::ApiToken, std::string>(core::ApiToken::Field::TokenPrefix) ==
        token_prefix));
    rtxn->commit();

    for (const auto& api_token : tokens) {
      if (api_token.revoked_at.has_value()) {
        continue;
      }
      if (!verify_token_hash(full, api_token.token_hash)) {
        continue;
      }
      if (api_token.expires_at.has_value() && now_ts() >= *api_token.expires_at) {
        throw TokenExpired("API token has expired");
      }
      return AuthToken{
          .session_id = core::SessionId(api_token.id.value()),
          .plaintext_token = std::string(full),
          .mfa_complete = true,
      };
    }

    throw InvalidCredentials("invalid or revoked API token");
  }

  // ---- Private: validate_session_token ----

  SessionContext LocalAuthProvider::validate_session_token(std::string_view bearer_token) {
    if (bearer_token.size() < config_.token_prefix_len) {
      throw InvalidCredentials("invalid token");
    }
    const auto token_prefix = prefix_of(bearer_token);

    auto rtxn = backend_.begin(storage::IsolationLevel::ReadCommitted);
    const auto sessions = rtxn->repo<core::Session>().query(storage::Query<core::Session>::where(
        storage::field<core::Session, std::string>(core::Session::Field::TokenPrefix) ==
        token_prefix));
    rtxn->commit();

    for (const auto& session : sessions) {
      if (session.revoked_at.has_value()) {
        continue;
      }
      if (!verify_token_hash(bearer_token, session.token_hash)) {
        continue;
      }

      // Verify the owning user is still active (runs before cache lookup).
      // The same row carries authz_version, used below to invalidate a stale
      // permission cache without waiting for the TTL.
      std::int64_t authz_version = 0;
      {
        auto user_rtxn = backend_.begin(storage::IsolationLevel::ReadCommitted);
        const auto users = user_rtxn->repo<core::User>().query(storage::Query<core::User>::where(
            storage::field<core::User, std::string>(core::User::Field::Id) ==
            session.user_id.to_string()));
        user_rtxn->commit();
        if (users.empty() || users.front().status != core::UserStatus::Active) {
          throw InvalidCredentials("user account is not active");
        }
        authz_version = users.front().authz_version;
      }

      const auto now = now_ts();
      check_session_expiry(session, now); // throws TokenExpired on violation
      update_last_seen_if_needed(session, now);
      return lookup_or_build_context(session, authz_version);
    }

    throw InvalidCredentials("invalid or expired session token");
  }

  // ---- Private: check_session_expiry ----

  void LocalAuthProvider::check_session_expiry(const core::Session& session,
                                               core::Timestamp now) const {
    const auto idle_micros = now.unix_micros() - session.last_seen_at.unix_micros();
    if (idle_micros > config_.max_session_idle_seconds * 1'000'000LL) {
      throw TokenExpired("session idle timeout exceeded");
    }
    const auto abs_micros = now.unix_micros() - session.created_at.unix_micros();
    if (abs_micros > config_.max_session_abs_seconds * 1'000'000LL) {
      throw TokenExpired("session absolute timeout exceeded");
    }
  }

  // ---- Private: update_last_seen_if_needed ----

  void LocalAuthProvider::update_last_seen_if_needed(const core::Session& session,
                                                     core::Timestamp now) {
    const auto idle_micros = now.unix_micros() - session.last_seen_at.unix_micros();
    if (idle_micros < config_.last_seen_update_interval_seconds * 1'000'000LL) {
      return; // within rate-limit window; skip update
    }
    try {
      auto updated = session;
      updated.last_seen_at = now;
      auto wtxn = backend_.begin(storage::IsolationLevel::Serializable);
      wtxn->repo<core::Session>().update(updated, system_ctx("update_last_seen"));
      wtxn->commit();
      // NOLINTNEXTLINE(bugprone-empty-catch)
    } catch (const storage::BackendError&) {
      // Non-fatal: a race between concurrent requests may cause a conflict; ignore.
    }
  }

  // ---- Private: lookup_or_build_context ----

  SessionContext LocalAuthProvider::lookup_or_build_context(const core::Session& session,
                                                            std::int64_t authz_version) {
    // MFA flag always comes from the live DB session row, never from the cache.
    const std::string session_key = session.id.to_string();

    if (config_.session_ctx_cache_ttl_seconds > 0) {
      std::scoped_lock guard(cache_mutex_);
      const auto iter = ctx_cache_.find(session_key);
      if (iter != ctx_cache_.end()) {
        const auto age = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::steady_clock::now() - iter->second.cached_at)
                             .count();
        // A cache hit must be both within the TTL and at the current authz epoch;
        // a permission change bumps the user's authz_version and forces a rebuild.
        if (age < config_.session_ctx_cache_ttl_seconds &&
            iter->second.authz_version == authz_version) {
          return SessionContext{
              .session_id = session.id,
              .user_id = session.user_id,
              .permissions_by_lab = iter->second.permissions_by_lab,
              .global_permissions = iter->second.global_permissions,
              .mfa_complete = session.mfa_complete,
          };
        }
        ctx_cache_.erase(iter); // stale (expired or superseded epoch)
      }
    }

    SessionContext ctx = build_session_context(session);
    if (config_.session_ctx_cache_ttl_seconds > 0) {
      std::scoped_lock guard(cache_mutex_);
      ctx_cache_.insert_or_assign(session_key, CachedContext{
                                                   .user_id = session.user_id,
                                                   .permissions_by_lab = ctx.permissions_by_lab,
                                                   .global_permissions = ctx.global_permissions,
                                                   .cached_at = std::chrono::steady_clock::now(),
                                                   .authz_version = authz_version,
                                               });
    }
    return ctx;
  }

  // ---- Private: validate_api_token ----

  SessionContext LocalAuthProvider::validate_api_token(std::string_view bearer_token) {
    if (!bearer_token.starts_with("fmgr_pat_") ||
        bearer_token.size() < 9 + config_.token_prefix_len) {
      throw InvalidCredentials("malformed API token");
    }
    const auto token_prefix = prefix_of(bearer_token);

    auto rtxn = backend_.begin(storage::IsolationLevel::ReadCommitted);
    const auto tokens = rtxn->repo<core::ApiToken>().query(storage::Query<core::ApiToken>::where(
        storage::field<core::ApiToken, std::string>(core::ApiToken::Field::TokenPrefix) ==
        token_prefix));
    rtxn->commit();

    for (const auto& api_token : tokens) {
      if (api_token.revoked_at.has_value()) {
        continue;
      }
      if (!verify_token_hash(bearer_token, api_token.token_hash)) {
        continue;
      }
      if (api_token.expires_at.has_value() && now_ts() >= *api_token.expires_at) {
        throw TokenExpired("API token has expired");
      }
      return build_api_token_context(api_token);
    }

    throw InvalidCredentials("invalid or revoked API token");
  }

  // ---- Private: build_session_context ----

  SessionContext LocalAuthProvider::build_session_context(const core::Session& session) const {
    auto rtxn = backend_.begin(storage::IsolationLevel::ReadCommitted);
    auto grants = resolve_permissions(*rtxn, session.user_id);
    rtxn->commit();

    return SessionContext{
        .session_id = session.id,
        .user_id = session.user_id,
        .permissions_by_lab = std::move(grants.permissions_by_lab),
        .global_permissions = std::move(grants.global_permissions),
        .mfa_complete = session.mfa_complete,
    };
  }

  // ---- Private: build_api_token_context ----

  SessionContext LocalAuthProvider::build_api_token_context(const core::ApiToken& token) const {
    auto rtxn = backend_.begin(storage::IsolationLevel::ReadCommitted);

    // Verify user is still active before granting any access.
    const auto users = rtxn->repo<core::User>().query(storage::Query<core::User>::where(
        storage::field<core::User, std::string>(core::User::Field::Id) ==
        token.user_id.to_string()));
    if (users.empty() || users.front().status != core::UserStatus::Active) {
      rtxn->commit();
      throw InvalidCredentials("user account is not active");
    }

    auto grants = resolve_permissions(*rtxn, token.user_id);
    rtxn->commit();

    // Intersect with the token's permission scope.
    const auto scoped = permissions_from_scope_json(token.scope_json);
    grants = intersect_grants_with_scope(std::move(grants), scoped);

    // Restrict to the token's designated lab (drops access to all other labs).
    if (token.lab_id.has_value()) {
      ScopedPermissionGrants lab_restricted;
      auto it = grants.permissions_by_lab.find(*token.lab_id);
      if (it != grants.permissions_by_lab.end()) {
        lab_restricted.permissions_by_lab[*token.lab_id] = std::move(it->second);
      }
      grants = std::move(lab_restricted);
    }

    return SessionContext{
        .session_id = core::SessionId(token.id.value()),
        .user_id = token.user_id,
        .permissions_by_lab = std::move(grants.permissions_by_lab),
        .global_permissions = std::move(grants.global_permissions),
        .mfa_complete = true,
    };
  }

  // ---- Private: user_has_totp ----

  bool LocalAuthProvider::user_has_totp(const core::UserId& uid) const {
    auto rtxn = backend_.begin(storage::IsolationLevel::ReadCommitted);
    const auto users = rtxn->repo<core::User>().query(storage::Query<core::User>::where(
        storage::field<core::User, std::string>(core::User::Field::Id) == uid.to_string()));
    rtxn->commit();
    return !users.empty() && users.front().totp_secret_enc.has_value();
  }

  // ---- Private: token helpers ----

  // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
  std::string LocalAuthProvider::generate_token() const {
    std::array<unsigned char, 32> buf{};
    randombytes_buf(buf.data(), buf.size());
    return bytes_to_hex(buf.data(), buf.size());
  }

  // Hashes the hex part of the token (strips "fmgr_pat_" prefix if present).
  // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
  std::string LocalAuthProvider::hash_token(std::string_view token) const {
    const std::string_view hex_part = token.starts_with("fmgr_pat_") ? token.substr(9) : token;

    std::array<unsigned char, crypto_generichash_BYTES> hash{};
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    crypto_generichash(hash.data(), hash.size(),
                       reinterpret_cast<const unsigned char*>(hex_part.data()), hex_part.size(),
                       nullptr, 0);
    return bytes_to_hex(hash.data(), hash.size());
  }

  // Constant-time comparison via sodium_memcmp.
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  bool LocalAuthProvider::verify_token_hash(std::string_view token,
                                            std::string_view stored_hash) const {
    const auto computed = hash_token(token);
    if (computed.size() != stored_hash.size()) {
      return false;
    }
    return sodium_memcmp(computed.data(), stored_hash.data(), computed.size()) == 0;
  }

  std::string LocalAuthProvider::prefix_of(std::string_view token) const {
    const std::string_view hex = token.starts_with("fmgr_pat_") ? token.substr(9) : token;
    if (hex.size() < config_.token_prefix_len) {
      throw InvalidCredentials("token too short for prefix extraction");
    }
    return std::string(hex.substr(0, config_.token_prefix_len));
  }

  // ---- Private: make_session_id ----

  core::SessionId LocalAuthProvider::make_session_id() {
    std::array<std::uint8_t, 16> bytes{};
    randombytes_buf(bytes.data(), bytes.size());
    bytes.at(6) = static_cast<std::uint8_t>((bytes.at(6) & 0x0FU) | 0x40U); // version 4
    bytes.at(8) = static_cast<std::uint8_t>((bytes.at(8) & 0x3FU) | 0x80U); // variant 1
    return core::SessionId(core::Uuid(bytes));
  }

  // ---- Private: system_ctx ----

  // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
  storage::MutationContext LocalAuthProvider::system_ctx(std::string_view reason) const {
    return storage::MutationContext{
        .actor_user_id = core::UserId::parse("00000000-0000-0000-0000-000000000000"),
        .actor_session_id = "system",
        .request_id = "auth",
        .reason = std::string(reason),
    };
  }

  // ---- Private: now_ts ----

  core::Timestamp LocalAuthProvider::now_ts() {
    return now_timestamp();
  }

  // ---- Private: cache helpers ----

  void LocalAuthProvider::cache_evict(const std::string& session_id_str) {
    std::scoped_lock guard(cache_mutex_);
    ctx_cache_.erase(session_id_str);
  }

  void LocalAuthProvider::cache_evict_user(const core::UserId& uid) {
    std::scoped_lock guard(cache_mutex_);
    for (auto iter = ctx_cache_.begin(); iter != ctx_cache_.end();) {
      if (iter->second.user_id == uid) {
        iter = ctx_cache_.erase(iter);
      } else {
        ++iter;
      }
    }
  }

  // ---- Private: lockout helpers ----

  void LocalAuthProvider::record_failure(const std::string& lower_email) {
    std::scoped_lock lock(lockout_mutex_);
    auto& state = lockout_map_[lower_email];
    ++state.failure_count;
    if (state.failure_count >= config_.max_failures_before_lockout) {
      const auto unlock_micros = std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count() +
                                 (config_.lockout_duration_seconds * 1'000'000LL);
      state.locked_until = core::Timestamp::from_unix_micros(unlock_micros);
    }
  }

  void LocalAuthProvider::record_success(const std::string& lower_email) {
    std::scoped_lock lock(lockout_mutex_);
    lockout_map_.erase(lower_email);
  }

  std::optional<core::Timestamp>
  LocalAuthProvider::check_lockout(const std::string& lower_email) const {
    std::scoped_lock lock(lockout_mutex_);
    const auto iter = lockout_map_.find(lower_email);
    if (iter == lockout_map_.end() || !iter->second.locked_until.has_value()) {
      return std::nullopt;
    }
    // has_value() was checked above; dereference is safe.
    const core::Timestamp until =
        iter->second.locked_until.value(); // NOLINT(bugprone-unchecked-optional-access)
    if (now_timestamp() >= until) {
      return std::nullopt;
    }
    return until;
  }

  // ---- create_api_token / revoke_api_token ----

  ApiTokenResult LocalAuthProvider::create_api_token(const core::UserId& user_id,
                                                     const std::string& name,
                                                     const std::string& scope_json,
                                                     std::optional<core::LabId> lab_id,
                                                     std::optional<core::Timestamp> expires_at,
                                                     const storage::MutationContext& ctx) {
    const auto hex = generate_token();
    const auto plaintext = "fmgr_pat_" + hex;
    const auto prefix = prefix_of(plaintext);
    const auto token_hash = hash_token(plaintext);

    const auto now = now_ts();
    const auto resolved_expires =
        expires_at.has_value() ? expires_at
                               : std::optional<core::Timestamp>{core::Timestamp::from_unix_micros(
                                     now.unix_micros() + 30LL * 24 * 3600 * 1000000)};

    // Re-use make_session_id() UUID generation; same random UUID logic works for ApiTokenId.
    std::array<std::uint8_t, 16> bytes{};
    randombytes_buf(bytes.data(), bytes.size());
    bytes.at(6) = static_cast<std::uint8_t>((bytes.at(6) & 0x0FU) | 0x40U);
    bytes.at(8) = static_cast<std::uint8_t>((bytes.at(8) & 0x3FU) | 0x80U);
    const auto api_token_id = core::ApiTokenId(core::Uuid(bytes));

    const core::ApiToken entity{
        .id = api_token_id,
        .user_id = user_id,
        .lab_id = lab_id,
        .name = name,
        .scope_json = scope_json,
        .token_hash = token_hash,
        .token_prefix = prefix,
        .created_at = now,
        .expires_at = resolved_expires,
    };

    auto wtxn = backend_.begin(storage::IsolationLevel::Serializable);
    wtxn->repo<core::ApiToken>().insert(entity, ctx);
    wtxn->commit();

    return ApiTokenResult{
        .api_token_id = api_token_id,
        .plaintext_token = plaintext,
        .token_prefix = prefix,
    };
  }

  void LocalAuthProvider::revoke_api_token(const core::ApiTokenId& api_token_id,
                                           const storage::MutationContext& ctx) {
    auto wtxn = backend_.begin(storage::IsolationLevel::Serializable);
    wtxn->repo<core::ApiToken>().soft_delete(api_token_id, ctx);
    wtxn->commit();
  }

} // namespace fmgr::auth
