// SPDX-License-Identifier: AGPL-3.0-or-later

#include "auth/MfaPolicy.h"
#include "auth/Totp.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fmgr::auth {
  namespace {

    constexpr std::string_view k_rfc_secret = "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ";
    constexpr std::int64_t k_micros_per_second = 1'000'000;

    [[nodiscard]] constexpr core::Timestamp at_seconds(std::int64_t seconds) {
      return core::Timestamp::from_unix_micros(seconds * k_micros_per_second);
    }

    TEST(TotpMatchingCountersTest, ReturnsThePreviousCurrentAndFutureCounter) {
      constexpr std::int64_t now = 1'234'567'890;
      for (const std::int64_t offset : {-1, 0, 1}) {
        const auto code = totp_generate(k_rfc_secret, now + offset * 30);
        EXPECT_EQ(totp_matching_counters(k_rfc_secret, code, now),
                  (std::vector<std::int64_t>{now / 30 + offset}));
      }
    }

    TEST(TotpMatchingCountersTest, CollectsEveryCoincidentMatchWithoutStoppingAtTheFirst) {
      // Fixed RFC-secret HOTP collision, independently calculated with HMAC-SHA1.
      EXPECT_EQ(totp_generate(k_rfc_secret, 910'737 * 30), "911617");
      EXPECT_EQ(totp_generate(k_rfc_secret, 910'738 * 30), "911617");
      EXPECT_EQ(totp_matching_counters(k_rfc_secret, "911617", 910'737 * 30),
                (std::vector<std::int64_t>{910'737, 910'738}));
    }

    TEST(TotpMatchingCountersTest, SkipsNegativeCounterAndPinsStepBoundary) {
      const auto first_code = totp_generate(k_rfc_secret, 0);
      EXPECT_EQ(totp_matching_counters(k_rfc_secret, first_code, 0),
                (std::vector<std::int64_t>{0}));
      EXPECT_EQ(totp_matching_counters(k_rfc_secret, first_code, 59),
                (std::vector<std::int64_t>{0}));
      EXPECT_TRUE(totp_matching_counters(k_rfc_secret, first_code, 60).empty());
    }

    TEST(TotpMatchingCountersTest, EvaluatesFutureCounterAtMaximumSignedTimeWithoutOverflow) {
      constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
      // HOTP(counter=INT64_MAX/30+1), calculated without counter*30 conversion.
      EXPECT_EQ(totp_matching_counters(k_rfc_secret, "704075", maximum),
                (std::vector<std::int64_t>{maximum / 30 + 1}));
    }

    TEST(TotpMatchingCountersTest, RejectsMalformedSubmittedCodesWithoutTrimming) {
      const std::array<std::string, 10> codes{
          "", "12345", "1234567", "abcdef", " 12345", "12345 ",
          "12345\n", std::string("123\0\x34\x35", 6), "１２３４５６", "١٢٣٤٥٦"};
      for (const auto& code : codes) {
        EXPECT_TRUE(totp_matching_counters(k_rfc_secret, code, 1'234'567'890).empty());
      }
    }

    TEST(TotpMatchingCountersTest, WrongCodeHasNoMatchingCounter) {
      EXPECT_TRUE(totp_matching_counters(k_rfc_secret, "000000", 1'234'567'890).empty());
    }

    TEST(TotpMatchingCountersTest, ValidatesOnlyTheAcceptedLocalSecurityProfile) {
      EXPECT_NO_THROW(validate_local_mfa_profile(TotpConfig{}));
      const std::array<TotpConfig, 8> invalid{
          TotpConfig{.step_seconds = 0}, TotpConfig{.step_seconds = -1},
          TotpConfig{.step_seconds = 1}, TotpConfig{.window_steps = 0},
          TotpConfig{.window_steps = -1}, TotpConfig{.window_steps = 2},
          TotpConfig{.digits = 8}, TotpConfig{.digits = 0}};
      for (const auto& config : invalid) {
        EXPECT_THROW(validate_local_mfa_profile(config), std::invalid_argument);
        EXPECT_THROW((void)totp_matching_counters(k_rfc_secret, "287082", 59, config),
                     std::invalid_argument);
      }
    }

    TEST(TotpMatchingCountersTest, RejectsNegativeTimeBeforeVerifyingAnyCode) {
      EXPECT_THROW((void)totp_matching_counters(k_rfc_secret, "287082", -1),
                   std::invalid_argument);
      EXPECT_THROW((void)totp_matching_counters(k_rfc_secret, "287082",
                                               std::numeric_limits<std::int64_t>::min()),
                   std::invalid_argument);
    }

    TEST(TotpMatchingCountersTest, RejectsMalformedStoredSecretsEvenWithMalformedSubmittedCode) {
      const std::array<std::string_view, 12> secrets{
          "", "=", "========", "A", "AAAA0AAA", "AAAA!AAA", "MY=bad",
          "MY=====", "MY=======", "MZ", "MY AAA", "MY======A"};
      for (const auto secret : secrets) {
        EXPECT_THROW((void)totp_matching_counters(secret, "not-a-code", 59),
                     std::invalid_argument);
      }
    }

    TEST(TotpMatchingCountersTest, AcceptsCaseInsensitiveOptionalCanonicalPadding) {
      const auto code = totp_generate("MY", 59);
      EXPECT_EQ(totp_matching_counters("MY", code, 59),
                totp_matching_counters("my======", code, 59));
    }

    TEST(MfaReplayPolicyTest, ConsumesMaximumOnlyWhenEveryMatchIsNew) {
      EXPECT_EQ(accepted_totp_step({91, 92}, std::nullopt), 92);
      EXPECT_EQ(accepted_totp_step({92, 91}, 90), 92);
      EXPECT_EQ(accepted_totp_step({0}, std::nullopt), 0);
      EXPECT_FALSE(accepted_totp_step({}, std::nullopt).has_value());
      EXPECT_FALSE(accepted_totp_step({91, 92}, 91).has_value());
      EXPECT_FALSE(accepted_totp_step({91, 92}, 92).has_value());
      EXPECT_FALSE(accepted_totp_step({91, 92}, 99).has_value());
    }

    TEST(MfaReplayPolicyTest, LaterIndependentNumericCollisionRemainsEligible) {
      // Replay state is the counter floor, never a global numeric-code blacklist.
      EXPECT_EQ(accepted_totp_step({200}, 100), 200);
    }

    TEST(MfaReplayPolicyTest, RejectsCorruptNegativeCountersOrFloor) {
      EXPECT_THROW((void)accepted_totp_step({-1, 2}, std::nullopt), std::invalid_argument);
      EXPECT_THROW((void)accepted_totp_step({1}, -1), std::invalid_argument);
    }

    TEST(MfaBackoffPolicyTest, PinsEveryFailureDelayAndSaturatesAtSeventeen) {
      constexpr std::array<std::int64_t, 18> delays{
          0, 0, 0, 0, 0, 30, 60, 120, 240, 480, 960, 1'920, 3'840, 7'680,
          15'360, 30'720, 61'440, 86'400};
      MfaPolicyState state{.last_accepted_step = 42};
      auto now = at_seconds(1'000);
      for (std::uint8_t count = 1; count <= 19; ++count) {
        state = mfa_record_failure(state, now);
        const auto expected_count = count < 17 ? count : std::uint8_t{17};
        EXPECT_EQ(state.failure_count, expected_count);
        EXPECT_EQ(state.last_accepted_step, 42);
        EXPECT_EQ(state.observed_at, now);
        const auto delay = delays.at(expected_count);
        if (delay == 0) {
          EXPECT_FALSE(state.next_allowed_at.has_value());
        } else {
          ASSERT_TRUE(state.next_allowed_at.has_value());
          EXPECT_EQ(state.next_allowed_at->unix_micros(),
                    now.unix_micros() + delay * k_micros_per_second);
          now = *state.next_allowed_at;
        }
      }
    }

    TEST(MfaBackoffPolicyTest, DeadlineMinusOneMicrosecondBlocksButEqualityIsEligible) {
      const MfaPolicyState state{
          .last_accepted_step = 10, .failure_count = 5,
          .next_allowed_at = at_seconds(130), .observed_at = at_seconds(100)};
      const auto before_deadline = core::Timestamp::from_unix_micros(
          state.next_allowed_at->unix_micros() - 1);
      EXPECT_TRUE(mfa_backoff_active(state, before_deadline));
      EXPECT_FALSE(mfa_backoff_active(state, *state.next_allowed_at));
      EXPECT_THROW((void)mfa_record_failure(state, before_deadline), std::logic_error);
      EXPECT_THROW((void)mfa_record_success(state, {11}, before_deadline), std::logic_error);
      EXPECT_EQ(mfa_record_failure(state, *state.next_allowed_at).failure_count, 6);
      EXPECT_EQ(state.failure_count, 5);
    }

    TEST(MfaBackoffPolicyTest, BackwardsClockCannotLowerObservedTimeOrPersistedDeadline) {
      const MfaPolicyState state{.last_accepted_step = 100, .failure_count = 4,
                                .observed_at = at_seconds(500)};
      EXPECT_EQ(mfa_effective_now(state, at_seconds(100)), at_seconds(500));
      const auto failed = mfa_record_failure(state, at_seconds(100));
      EXPECT_EQ(failed.observed_at, at_seconds(500));
      EXPECT_EQ(failed.next_allowed_at, at_seconds(530));
      EXPECT_TRUE(mfa_backoff_active(failed, at_seconds(200)));
    }

    TEST(MfaBackoffPolicyTest, ForwardClockAndLongInactivityDoNotReplenishTheBudget) {
      const MfaPolicyState state{
          .last_accepted_step = 100, .failure_count = 16,
          .next_allowed_at = at_seconds(10), .observed_at = at_seconds(1)};
      const auto failed = mfa_record_failure(state, at_seconds(1'000'000));
      EXPECT_EQ(failed.failure_count, 17);
      EXPECT_EQ(failed.next_allowed_at, at_seconds(1'086'400));
      EXPECT_EQ(failed.last_accepted_step, 100);
    }

    TEST(MfaBackoffPolicyTest, SuccessAdvancesFloorAndResetsOnlyFailureAllowance) {
      const MfaPolicyState state{
          .last_accepted_step = 100, .failure_count = 5,
          .next_allowed_at = at_seconds(100), .observed_at = at_seconds(90)};
      const auto succeeded = mfa_record_success(state, {101, 102}, at_seconds(100));
      EXPECT_EQ(succeeded.last_accepted_step, 102);
      EXPECT_EQ(succeeded.failure_count, 0);
      EXPECT_FALSE(succeeded.next_allowed_at.has_value());
      EXPECT_EQ(succeeded.observed_at, at_seconds(100));
      EXPECT_THROW((void)mfa_record_success(state, {100, 101}, at_seconds(100)),
                   std::invalid_argument);
      EXPECT_THROW((void)mfa_record_success(state, {}, at_seconds(100)), std::invalid_argument);
    }

    TEST(MfaBackoffPolicyTest, UnlockPreservesFloorAndObservedTimeEvenDuringBackoff) {
      const MfaPolicyState state{
          .last_accepted_step = 100, .failure_count = 17,
          .next_allowed_at = at_seconds(86'900), .observed_at = at_seconds(500)};
      const auto unlocked = mfa_unlock(state, at_seconds(100));
      EXPECT_EQ(unlocked.last_accepted_step, 100);
      EXPECT_EQ(unlocked.failure_count, 0);
      EXPECT_FALSE(unlocked.next_allowed_at.has_value());
      EXPECT_EQ(unlocked.observed_at, at_seconds(500));
      EXPECT_FALSE(accepted_totp_step({100}, unlocked.last_accepted_step).has_value());
      EXPECT_EQ(mfa_unlock(unlocked, at_seconds(100)), unlocked);
    }

    TEST(MfaBackoffPolicyTest, UnrepresentableDeadlineFailsWithoutWrappingOrMutatingInput) {
      const MfaPolicyState state{.last_accepted_step = 100, .failure_count = 4};
      const auto maximum = core::Timestamp::from_unix_micros(
          std::numeric_limits<std::int64_t>::max());
      EXPECT_THROW((void)mfa_record_failure(state, maximum), std::overflow_error);
      EXPECT_EQ(state.failure_count, 4);
      EXPECT_FALSE(state.next_allowed_at.has_value());
      EXPECT_FALSE(state.observed_at.has_value());
    }

    TEST(MfaBackoffPolicyTest, RejectsMalformedPersistedStateAndNegativeWallTime) {
      const auto negative = core::Timestamp::from_unix_micros(-1);
      const std::array<MfaPolicyState, 4> invalid{
          MfaPolicyState{.last_accepted_step = -1}, MfaPolicyState{.failure_count = 18},
          MfaPolicyState{.next_allowed_at = negative}, MfaPolicyState{.observed_at = negative}};
      for (const auto& state : invalid) {
        EXPECT_THROW((void)mfa_effective_now(state, at_seconds(1)), std::invalid_argument);
        EXPECT_THROW((void)mfa_backoff_active(state, at_seconds(1)), std::invalid_argument);
        EXPECT_THROW((void)mfa_record_failure(state, at_seconds(1)), std::invalid_argument);
        EXPECT_THROW((void)mfa_record_success(state, {1}, at_seconds(1)), std::invalid_argument);
        EXPECT_THROW((void)mfa_unlock(state, at_seconds(1)), std::invalid_argument);
      }
      EXPECT_THROW((void)mfa_effective_now(MfaPolicyState{}, negative), std::invalid_argument);
    }

  } // namespace
} // namespace fmgr::auth
