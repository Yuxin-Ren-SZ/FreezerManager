// SPDX-License-Identifier: AGPL-3.0-or-later

#include "auth/MfaPolicy.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace fmgr::auth {
  namespace {

    constexpr std::uint8_t k_max_failure_count = 17;
    constexpr std::int64_t k_micros_per_second = 1'000'000;

    [[nodiscard]] std::int64_t failure_delay_seconds(std::uint8_t count) {
      if (count < 5) {
        return 0;
      }
      if (count == k_max_failure_count) {
        return 86'400;
      }
      return 30LL << static_cast<unsigned>(count - 5);
    }

    void validate_state(const MfaPolicyState& state) {
      if (state.failure_count > k_max_failure_count || state.last_accepted_step.value_or(0) < 0 ||
          (state.next_allowed_at.has_value() && state.next_allowed_at->unix_micros() < 0) ||
          (state.observed_at.has_value() && state.observed_at->unix_micros() < 0) ||
          (state.last_accepted_step.has_value() && !state.observed_at.has_value()) ||
          (state.failure_count > 0 && !state.observed_at.has_value())) {
        throw std::invalid_argument("invalid MFA policy state");
      }
      const auto delay_micros = failure_delay_seconds(state.failure_count) * k_micros_per_second;
      if (delay_micros == 0) {
        if (state.next_allowed_at.has_value()) {
          throw std::invalid_argument("invalid MFA policy deadline");
        }
        return;
      }
      const auto observed_micros = state.observed_at.value_or(core::Timestamp{}).unix_micros();
      if (!state.next_allowed_at.has_value() ||
          observed_micros > std::numeric_limits<std::int64_t>::max() - delay_micros ||
          state.next_allowed_at->unix_micros() != observed_micros + delay_micros) {
        throw std::invalid_argument("invalid MFA policy deadline");
      }
    }

    [[nodiscard]] core::Timestamp add_delay(core::Timestamp now, std::int64_t seconds) {
      const auto micros = seconds * k_micros_per_second;
      if (now.unix_micros() > std::numeric_limits<std::int64_t>::max() - micros) {
        throw std::overflow_error("MFA deadline is not representable");
      }
      return core::Timestamp::from_unix_micros(now.unix_micros() + micros);
    }

    [[nodiscard]] core::Timestamp eligible_time(const MfaPolicyState& state,
                                                core::Timestamp wall_now) {
      const auto now = mfa_effective_now(state, wall_now);
      if (state.next_allowed_at.has_value() && now < *state.next_allowed_at) {
        throw std::logic_error("MFA backoff is active");
      }
      return now;
    }

  } // namespace

  std::optional<std::int64_t> accepted_totp_step(const std::vector<std::int64_t>& matches,
                                                 std::optional<std::int64_t> last_accepted_step) {
    if (last_accepted_step.value_or(0) < 0 ||
        std::ranges::any_of(matches, [](std::int64_t step) { return step < 0; })) {
      throw std::invalid_argument("invalid TOTP counter state");
    }
    const auto floor = last_accepted_step.value_or(-1);
    if (matches.empty() ||
        std::ranges::any_of(matches, [floor](std::int64_t step) { return step <= floor; })) {
      return std::nullopt;
    }
    return std::ranges::max(matches);
  }

  core::Timestamp mfa_effective_now(const MfaPolicyState& state, core::Timestamp wall_now) {
    validate_state(state);
    if (wall_now.unix_micros() < 0) {
      throw std::invalid_argument("invalid MFA wall time");
    }
    return state.observed_at.has_value() ? std::max(wall_now, *state.observed_at) : wall_now;
  }

  bool mfa_backoff_active(const MfaPolicyState& state, core::Timestamp wall_now) {
    const auto now = mfa_effective_now(state, wall_now);
    return state.next_allowed_at.has_value() && now < *state.next_allowed_at;
  }

  MfaPolicyState mfa_record_failure(const MfaPolicyState& state, core::Timestamp wall_now) {
    const auto now = eligible_time(state, wall_now);
    auto next = state;
    if (next.failure_count < k_max_failure_count) {
      ++next.failure_count;
    }
    const auto delay = failure_delay_seconds(next.failure_count);
    next.next_allowed_at = delay == 0 ? std::nullopt : std::optional{add_delay(now, delay)};
    next.observed_at = now;
    return next;
  }

  MfaPolicyState mfa_record_success(const MfaPolicyState& state,
                                    const std::vector<std::int64_t>& matching_steps,
                                    core::Timestamp wall_now) {
    const auto now = eligible_time(state, wall_now);
    const auto accepted = accepted_totp_step(matching_steps, state.last_accepted_step);
    if (!accepted.has_value()) {
      throw std::invalid_argument("TOTP counters are not fresh");
    }
    auto next = state;
    next.last_accepted_step = accepted;
    next.failure_count = 0;
    next.next_allowed_at.reset();
    next.observed_at = now;
    return next;
  }

  MfaPolicyState mfa_unlock(const MfaPolicyState& state, core::Timestamp wall_now) {
    auto next = state;
    next.observed_at = mfa_effective_now(state, wall_now);
    next.failure_count = 0;
    next.next_allowed_at.reset();
    return next;
  }

} // namespace fmgr::auth
