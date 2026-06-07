// SPDX-License-Identifier: AGPL-3.0-or-later

// D9: Server-side session and API-token domain types.
//
// Sessions are opaque: the server stores a token_hash (Argon2id) and a
// plaintext token_prefix for indexed lookup.  The auth layer generates the
// full random token, hashes it, and stores only the hash here.  The prefix
// is not a secret — it is used only to narrow the lookup before the hash
// comparison.
//
// ApiToken follows the same prefix+hash scheme and additionally carries a
// lab_id scope, a human-readable name, and an expiry timestamp.  Expiry
// enforcement is the auth layer's responsibility; the repository records
// whatever it is given.
//
// Both entities are soft-deleted by setting revoked_at_micros.  The default
// query filter excludes revoked rows; include_tombstoned() removes the filter.
//
// Rate-limiting the last_seen_at update (to avoid write amplification on every
// request) is the auth layer's responsibility; the repository stores whatever
// Timestamp it is given.
#ifndef FMGR_CORE_SESSION_H
#define FMGR_CORE_SESSION_H

#include "core/ids.h"
#include "core/json_helpers.h"
#include "core/timestamp.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace fmgr::core {

  // ---- Session ----

  struct Session {
    using Id = SessionId;

    enum class Field : std::uint8_t {
      Id,
      UserId,
      TokenHash,
      TokenPrefix,
      CreatedAt,
      LastSeenAt,
      Ip,
      UserAgent,
      RevokedAt,
      MfaComplete,
    };

    SessionId id;
    UserId user_id;
    std::string token_hash;   // BLAKE2b-256 hash; faster than SHA-256 for high-freq token verification; never contains plaintext
    std::string token_prefix; // first N chars of plaintext token; indexed for lookup
    Timestamp created_at;
    Timestamp last_seen_at;
    std::optional<std::string> ip;
    std::optional<std::string> user_agent;
    std::optional<Timestamp> revoked_at; // tombstone — set by soft_delete()
    // false until verify_totp() succeeds for sessions that require MFA.
    // Middleware must reject sensitive RPCs when this flag is false.
    bool mfa_complete{true};

    friend bool operator==(const Session&, const Session&) = default;
  };

  // ---- ApiToken ----

  struct ApiToken {
    using Id = ApiTokenId;

    enum class Field : std::uint8_t {
      Id,
      UserId,
      LabId,
      Name,
      ScopeJson,
      TokenHash,
      TokenPrefix,
      CreatedAt,
      ExpiresAt,
      RevokedAt,
    };

    ApiTokenId id;
    UserId user_id;
    std::optional<LabId> lab_id;  // null = not scoped to a specific lab
    std::string name;             // user-visible label, e.g. "Jupyter notebook token"
    std::string scope_json{R"(["*"])"}; // ["*"] = unrestricted; [] = zero perms; explicit list = restricted
    std::string token_hash;
    std::string token_prefix;
    Timestamp created_at;
    std::optional<Timestamp> expires_at; // null = no hard expiry; auth layer defaults 30 d
    std::optional<Timestamp> revoked_at; // tombstone

    friend bool operator==(const ApiToken&, const ApiToken&) = default;
  };


  // ---- Session JSON ----

  inline void to_json(nlohmann::json& json, const Session& session) {
    json = nlohmann::json{
        {"id", session.id},
        {"user_id", session.user_id},
        {"token_hash", session.token_hash},
        {"token_prefix", session.token_prefix},
        {"created_at", session.created_at},
        {"last_seen_at", session.last_seen_at},
        {"ip", json_helpers::opt_to_json(session.ip)},
        {"user_agent", json_helpers::opt_to_json(session.user_agent)},
        {"revoked_at", json_helpers::opt_to_json(session.revoked_at)},
        {"mfa_complete", session.mfa_complete},
    };
  }

  inline void from_json(const nlohmann::json& json, Session& session) {
    session = Session{
        .id = json.at("id").get<SessionId>(),
        .user_id = json.at("user_id").get<UserId>(),
        .token_hash = json.at("token_hash").get<std::string>(),
        .token_prefix = json.at("token_prefix").get<std::string>(),
        .created_at = json.at("created_at").get<Timestamp>(),
        .last_seen_at = json.at("last_seen_at").get<Timestamp>(),
        .ip = json_helpers::opt_from_json<std::string>(json.at("ip")),
        .user_agent = json_helpers::opt_from_json<std::string>(json.at("user_agent")),
        .revoked_at = json_helpers::opt_from_json<Timestamp>(json.at("revoked_at")),
        .mfa_complete = json.at("mfa_complete").get<bool>(),
    };
  }

  // ---- ApiToken JSON ----

  inline void to_json(nlohmann::json& json, const ApiToken& token) {
    json = nlohmann::json{
        {"id", token.id},
        {"user_id", token.user_id},
        {"lab_id", json_helpers::opt_to_json(token.lab_id)},
        {"name", token.name},
        {"scope_json", token.scope_json},
        {"token_hash", token.token_hash},
        {"token_prefix", token.token_prefix},
        {"created_at", token.created_at},
        {"expires_at", json_helpers::opt_to_json(token.expires_at)},
        {"revoked_at", json_helpers::opt_to_json(token.revoked_at)},
    };
  }

  inline void from_json(const nlohmann::json& json, ApiToken& token) {
    token = ApiToken{
        .id = json.at("id").get<ApiTokenId>(),
        .user_id = json.at("user_id").get<UserId>(),
        .lab_id = json_helpers::opt_from_json<LabId>(json.at("lab_id")),
        .name = json.at("name").get<std::string>(),
        .scope_json = json.at("scope_json").get<std::string>(),
        .token_hash = json.at("token_hash").get<std::string>(),
        .token_prefix = json.at("token_prefix").get<std::string>(),
        .created_at = json.at("created_at").get<Timestamp>(),
        .expires_at = json_helpers::opt_from_json<Timestamp>(json.at("expires_at")),
        .revoked_at = json_helpers::opt_from_json<Timestamp>(json.at("revoked_at")),
    };
  }

} // namespace fmgr::core

#endif // FMGR_CORE_SESSION_H
