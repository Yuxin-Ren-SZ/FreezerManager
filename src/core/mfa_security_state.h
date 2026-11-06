// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_CORE_MFA_SECURITY_STATE_H
#define FMGR_CORE_MFA_SECURITY_STATE_H

#include "core/ids.h"
#include "core/json_helpers.h"
#include "core/timestamp.h"

#include <cstdint>
#include <optional>

namespace fmgr::core {

  // Private durable state: never serialized into a public RPC or user profile.
  struct MfaSecurityState {
    using Id = UserId;
    enum class Field : std::uint8_t {
      UserId,
      LastAcceptedStep,
      FailureCount,
      NextAllowedAt,
      ObservedAt,
      TotpGeneration
    };
    UserId user_id;
    std::optional<std::int64_t> last_accepted_step;
    std::int32_t failure_count{0};
    std::optional<Timestamp> next_allowed_at;
    std::optional<Timestamp> observed_at;
    std::optional<Uuid> totp_generation;

    friend bool operator==(const MfaSecurityState&, const MfaSecurityState&) = default;
  };

  inline void to_json(nlohmann::json& json, const MfaSecurityState& state) {
    json = nlohmann::json{
        {"user_id", state.user_id},
        {"last_accepted_step", json_helpers::opt_to_json(state.last_accepted_step)},
        {"failure_count", state.failure_count},
        {"next_allowed_at", json_helpers::opt_to_json(state.next_allowed_at)},
        {"observed_at", json_helpers::opt_to_json(state.observed_at)},
        {"totp_generation", json_helpers::opt_to_json(state.totp_generation)},
    };
  }

} // namespace fmgr::core
#endif
