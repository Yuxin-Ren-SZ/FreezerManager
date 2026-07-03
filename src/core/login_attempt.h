// SPDX-License-Identifier: AGPL-3.0-or-later

// C-1: Persistent login-lockout state.
//
// The LocalAuthProvider throttles password guessing by counting consecutive
// failed logins per email and locking the account for a configured window once
// a threshold is crossed. This state must survive a `freezerd` restart —
// otherwise an attacker bypasses the lockout by bouncing the process — so it is
// stored here rather than only in memory.
//
// One active row per email. A successful login soft-deletes (clears) the active
// row via `cleared_at`; the next failure inserts a fresh active row. A partial
// unique index on (email) WHERE cleared_at IS NULL enforces the single-active-row
// invariant while letting cleared rows accumulate (mirrors sessions/api_tokens).
#ifndef FMGR_CORE_LOGIN_ATTEMPT_H
#define FMGR_CORE_LOGIN_ATTEMPT_H

#include "core/ids.h"
#include "core/json_helpers.h"
#include "core/timestamp.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace fmgr::core {

  struct LoginAttempt {
    using Id = LoginAttemptId;

    enum class Field : std::uint8_t {
      Id,
      Email,
      FailureCount,
      LockedUntil,
      LastActivity,
      ClearedAt,
    };

    LoginAttemptId id;
    std::string email; // lowercased login identifier this counter tracks
    std::int64_t failure_count{0};
    std::optional<Timestamp> locked_until; // set once failure_count crosses the threshold
    Timestamp last_activity;               // time of the most recent recorded failure
    std::optional<Timestamp> cleared_at;   // tombstone — set on a successful login

    friend bool operator==(const LoginAttempt&, const LoginAttempt&) = default;
  };

  inline void to_json(nlohmann::json& json, const LoginAttempt& attempt) {
    json = nlohmann::json{
        {"id", attempt.id},
        {"email", attempt.email},
        {"failure_count", attempt.failure_count},
        {"locked_until", json_helpers::opt_to_json(attempt.locked_until)},
        {"last_activity", attempt.last_activity},
        {"cleared_at", json_helpers::opt_to_json(attempt.cleared_at)},
    };
  }

  inline void from_json(const nlohmann::json& json, LoginAttempt& attempt) {
    attempt = LoginAttempt{
        .id = json.at("id").get<LoginAttemptId>(),
        .email = json.at("email").get<std::string>(),
        .failure_count = json.at("failure_count").get<std::int64_t>(),
        .locked_until = json_helpers::opt_from_json<Timestamp>(json.at("locked_until")),
        .last_activity = json.at("last_activity").get<Timestamp>(),
        .cleared_at = json_helpers::opt_from_json<Timestamp>(json.at("cleared_at")),
    };
  }

} // namespace fmgr::core

#endif // FMGR_CORE_LOGIN_ATTEMPT_H
