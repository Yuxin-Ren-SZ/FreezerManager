// SPDX-License-Identifier: AGPL-3.0-or-later

// E2: Unit tests for the TOTP helper (Totp.h / Totp.cc).
//
// RFC 6238 SHA-1 known-answer test vectors:
//   Secret (raw bytes): "12345678901234567890" (20 bytes)
//   Base32 encoding:    "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ"
//   Vectors from RFC 6238 Appendix B (8-digit codes truncated to 6 digits):
//     T=59           → step 1         → "287082"
//     T=1111111109   → step 37037036  → "081804"
//     T=1111111111   → step 37037037  → "050471"
//     T=1234567890   → step 41152263  → "005924"
//     T=2000000000   → step 66666666  → "279037"

#include "auth/Totp.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace fmgr::auth {
  namespace {

    // RFC 6238 SHA-1 test secret in base32.
    constexpr std::string_view kRfc6238Secret = "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ";

    // ---- base32_decode ----

    TEST(TotpTest, Base32DecodeKnownVector) {
      // "GEZDGNBV" (first 8 chars of the RFC 6238 SHA-1 secret base32) decodes
      // to "12345" (0x31 0x32 0x33 0x34 0x35).  Verified by hand against the
      // full RFC 6238 secret "12345678901234567890".
      const auto bytes = base32_decode("GEZDGNBV");
      ASSERT_EQ(bytes.size(), 5U);
      EXPECT_EQ(bytes.at(0), 0x31U); // '1'
      EXPECT_EQ(bytes.at(1), 0x32U); // '2'
      EXPECT_EQ(bytes.at(2), 0x33U); // '3'
      EXPECT_EQ(bytes.at(3), 0x34U); // '4'
      EXPECT_EQ(bytes.at(4), 0x35U); // '5'
    }

    TEST(TotpTest, Base32DecodeLowerCase) {
      // base32_decode must be case-insensitive.
      const auto upper = base32_decode("GEZDGNBV");
      const auto lower = base32_decode("gezdgnbv");
      EXPECT_EQ(upper, lower);
    }

    TEST(TotpTest, Base32DecodePaddingStripped) {
      // With or without '=' padding must produce the same result.
      const auto without = base32_decode("MFRA");
      const auto with_pad = base32_decode("MFRA====");
      EXPECT_EQ(without, with_pad);
    }

    TEST(TotpTest, Base32DecodeInvalidCharThrows) {
      // NOLINTBEGIN(bugprone-unused-return-value)
      EXPECT_THROW((void)base32_decode("AAAA!AAA"), std::invalid_argument);
      EXPECT_THROW((void)base32_decode("AAAA0AAA"), std::invalid_argument); // '0' not in alphabet
      EXPECT_THROW((void)base32_decode("AAAA1AAA"), std::invalid_argument); // '1' not in alphabet
      // NOLINTEND(bugprone-unused-return-value)
    }

    // ---- RFC 6238 known-answer tests ----

    TEST(TotpTest, Rfc6238Vector_T59) {
      EXPECT_EQ(totp_generate(kRfc6238Secret, 59), "287082");
    }

    TEST(TotpTest, Rfc6238Vector_T1111111109) {
      EXPECT_EQ(totp_generate(kRfc6238Secret, 1111111109LL), "081804");
    }

    TEST(TotpTest, Rfc6238Vector_T1111111111) {
      EXPECT_EQ(totp_generate(kRfc6238Secret, 1111111111LL), "050471");
    }

    TEST(TotpTest, Rfc6238Vector_T1234567890) {
      EXPECT_EQ(totp_generate(kRfc6238Secret, 1234567890LL), "005924");
    }

    TEST(TotpTest, Rfc6238Vector_T2000000000) {
      EXPECT_EQ(totp_generate(kRfc6238Secret, 2000000000LL), "279037");
    }

    // ---- Window tests ----

    TEST(TotpTest, CurrentStepRoundTrip) {
      // A generated code for time T must verify at the same T.
      const std::int64_t t = 1234567890LL;
      const auto code = totp_generate(kRfc6238Secret, t);
      EXPECT_TRUE(totp_verify(kRfc6238Secret, code, t));
    }

    TEST(TotpTest, PreviousStepIsValid) {
      // A code generated for T-30 must verify at T (within the ±1 step window).
      const std::int64_t t = 1234567890LL;
      const auto code = totp_generate(kRfc6238Secret, t - 30);
      EXPECT_TRUE(totp_verify(kRfc6238Secret, code, t));
    }

    TEST(TotpTest, NextStepIsValid) {
      // A code generated for T+30 must verify at T.
      const std::int64_t t = 1234567890LL;
      const auto code = totp_generate(kRfc6238Secret, t + 30);
      EXPECT_TRUE(totp_verify(kRfc6238Secret, code, t));
    }

    TEST(TotpTest, TwoStepsOldIsInvalid) {
      // A code generated for T-60 must NOT verify at T (outside the ±1 window).
      const std::int64_t t = 1234567890LL;
      const auto code = totp_generate(kRfc6238Secret, t - 60);
      EXPECT_FALSE(totp_verify(kRfc6238Secret, code, t));
    }

    TEST(TotpTest, WrongCodeRejected) {
      EXPECT_FALSE(totp_verify(kRfc6238Secret, "000000", 1234567890LL));
    }

    TEST(TotpTest, Base32DecodeEmptyStringReturnsEmpty) {
      const auto bytes = base32_decode("");
      EXPECT_TRUE(bytes.empty());
    }

    TEST(TotpTest, TotpVerifyRejectsEmptyCode) {
      EXPECT_FALSE(totp_verify(kRfc6238Secret, "", 1234567890LL));
    }

    TEST(TotpTest, TotpVerifyRejectsTooShortCode) {
      EXPECT_FALSE(totp_verify(kRfc6238Secret, "12345", 1234567890LL));
    }

    TEST(TotpTest, TotpVerifyRejectsTooLongCode) {
      EXPECT_FALSE(totp_verify(kRfc6238Secret, "1234567", 1234567890LL));
    }

    TEST(TotpTest, CounterBelowZeroSkipInVerifyWindow) {
      // When now_unix_seconds is small enough that base_counter == 0, negative
      // deltas produce counter == -1. TOTP verify must skip negative counters
      // (RFC 6238 §5.2: counter must be ≥ 0) and still accept the current step.
      constexpr std::int64_t time_in_first_step = 15;
      const auto code = totp_generate(kRfc6238Secret, time_in_first_step);
      EXPECT_TRUE(totp_verify(kRfc6238Secret, code, time_in_first_step));
    }

  } // namespace
} // namespace fmgr::auth
