// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_AUTH_MFAPOLICY_H
#define FMGR_AUTH_MFAPOLICY_H

#include "core/timestamp.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace fmgr::auth {

  // Pure projection of durable per-user policy state. Enrollment generation and
  // user identity belong to the storage entity; they are not policy inputs.
  struct MfaPolicyState {
    std::optional<std::int64_t> last_accepted_step;
    std::uint8_t failure_count{0};
    std::optional<core::Timestamp> next_allowed_at;
    std::optional<core::Timestamp> observed_at;

    friend bool operator==(const MfaPolicyState&, const MfaPolicyState&) = default;
  };

  // Return max(matches) only if every counter is strictly above the old floor.
  // Empty/old matches reject. Negative counters/floors are invalid stored input.
  [[nodiscard]] std::optional<std::int64_t>
  accepted_totp_step(const std::vector<std::int64_t>& matches,
                     std::optional<std::int64_t> last_accepted_step);

  // All clocks are explicit. These functions reject negative time, negative
  // floors, counts outside 0..17 and state inconsistent with its derived
  // backoff deadline/high-water time with std::invalid_argument.
  [[nodiscard]] core::Timestamp mfa_effective_now(const MfaPolicyState& state,
                                                  core::Timestamp wall_now);
  [[nodiscard]] bool mfa_backoff_active(const MfaPolicyState& state, core::Timestamp wall_now);

  // These return proposed state, never persist it. The caller must commit the
  // transition with its audit and session effects before reporting an outcome.
  // During backoff, failure/success throw std::logic_error without evaluating
  // another guess. Success additionally requires wholly fresh matching steps.
  // Deadline overflow throws std::overflow_error; no wrapped deadline is returned.
  [[nodiscard]] MfaPolicyState mfa_record_failure(const MfaPolicyState& state,
                                                  core::Timestamp wall_now);
  [[nodiscard]] MfaPolicyState mfa_record_success(const MfaPolicyState& state,
                                                  const std::vector<std::int64_t>& matching_steps,
                                                  core::Timestamp wall_now);

  // Authorization for unlock is a provider/storage responsibility. This pure
  // transition resets only the guessing allowance and advances observed time.
  [[nodiscard]] MfaPolicyState mfa_unlock(const MfaPolicyState& state, core::Timestamp wall_now);

} // namespace fmgr::auth

#endif // FMGR_AUTH_MFAPOLICY_H
