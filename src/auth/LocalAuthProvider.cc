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

    // Parses a JSON array of permission-key strings into a permission set.
    [[nodiscard]] std::set<core::Permission>
    permissions_from_scope_json(const std::string& scope_json) {
      std::set<core::Permission> perms;
      const auto parsed = nlohmann::json::parse(scope_json, nullptr, false);
      if (!parsed.is_array()) {
        return perms;
      }
      for (const auto& item : parsed) {
        if (item.is_string()) {
          try {
            perms.insert(core::parse_permission(item.get<std::string>()));
            // NOLINTNEXTLINE(bugprone-empty-catch)
          } catch (const std::exception&) {
            // Unknown permission keys are silently skipped; never crash on bad scope_json.
          }
        }
      }
      return perms;
    }

    // Resolve effective permissions for a user from their lab memberships.
    [[nodiscard]] std::pair<std::vector<core::LabId>, std::set<core::Permission>>
    resolve_permissions(storage::ITransaction& txn, const core::UserId& user_id) {
      const auto memberships =
          txn.repo<core::LabMembership>().query(storage::Query<core::LabMembership>::where(
              storage::field<core::LabMembership, std::string>(
                  core::LabMembership::Field::UserId) == user_id.to_string()));

      std::vector<core::LabId> visible_labs;
      std::set<core::Permission> permissions;

      for (const auto& membership : memberships) {
        visible_labs.push_back(membership.lab_id);
        if (!membership.role_id.has_value()) {
          continue;
        }
        const auto role_perms =
            txn.repo<core::RolePermission>().query(storage::Query<core::RolePermission>::where(
                storage::field<core::RolePermission, std::string>(
                    core::RolePermission::Field::RoleId) == membership.role_id->to_string()));
        for (const auto& role_perm : role_perms) {
          permissions.insert(role_perm.permission);
        }
      }

      return {std::move(visible_labs), std::move(permissions)};
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
    if (users.empty() || !users.front().totp_secret_enc.has_value()) {
      txn->commit();
      throw InvalidCredentials("no TOTP secret configured for this user");
    }

    const auto& totp_secret = *users.front().totp_secret_enc;
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
      const auto now = now_ts();
      check_session_expiry(session, now); // throws TokenExpired on violation
      update_last_seen_if_needed(session, now);
      return lookup_or_build_context(session);
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

  SessionContext LocalAuthProvider::lookup_or_build_context(const core::Session& session) {
    // MFA flag always comes from the live DB session row, never from the cache.
    const std::string session_key = session.id.to_string();

    if (config_.session_ctx_cache_ttl_seconds > 0) {
      std::scoped_lock guard(cache_mutex_);
      const auto iter = ctx_cache_.find(session_key);
      if (iter != ctx_cache_.end()) {
        const auto age = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::steady_clock::now() - iter->second.cached_at)
                             .count();
        if (age < config_.session_ctx_cache_ttl_seconds) {
          return SessionContext{
              .session_id = session.id,
              .user_id = session.user_id,
              .visible_labs = iter->second.visible_labs,
              .permissions = iter->second.permissions,
              .mfa_complete = session.mfa_complete,
          };
        }
        ctx_cache_.erase(iter); // stale entry
      }
    }

    SessionContext ctx = build_session_context(session);
    if (config_.session_ctx_cache_ttl_seconds > 0) {
      std::scoped_lock guard(cache_mutex_);
      ctx_cache_.insert_or_assign(session_key, CachedContext{
                                                   .user_id = session.user_id,
                                                   .visible_labs = ctx.visible_labs,
                                                   .permissions = ctx.permissions,
                                                   .cached_at = std::chrono::steady_clock::now(),
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
    auto [visible_labs, permissions] = resolve_permissions(*rtxn, session.user_id);
    rtxn->commit();

    return SessionContext{
        .session_id = session.id,
        .user_id = session.user_id,
        .visible_labs = std::move(visible_labs),
        .permissions = std::move(permissions),
        .mfa_complete = session.mfa_complete,
    };
  }

  // ---- Private: build_api_token_context ----

  SessionContext LocalAuthProvider::build_api_token_context(const core::ApiToken& token) const {
    auto rtxn = backend_.begin(storage::IsolationLevel::ReadCommitted);
    auto [visible_labs, base_permissions] = resolve_permissions(*rtxn, token.user_id);
    rtxn->commit();

    // Intersect base permissions with the token's own scope_json (if non-empty).
    const auto scoped = permissions_from_scope_json(token.scope_json);
    std::set<core::Permission> permissions;
    if (scoped.empty()) {
      permissions = std::move(base_permissions);
    } else {
      for (const auto& perm : scoped) {
        if (base_permissions.contains(perm)) {
          permissions.insert(perm);
        }
      }
    }

    return SessionContext{
        .session_id = core::SessionId(token.id.value()),
        .user_id = token.user_id,
        .visible_labs = std::move(visible_labs),
        .permissions = std::move(permissions),
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

} // namespace fmgr::auth
