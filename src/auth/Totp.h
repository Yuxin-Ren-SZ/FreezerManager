// SPDX-License-Identifier: AGPL-3.0-or-later

// E2: TOTP (RFC 6238) helper — time-based one-time passwords.
//
// Algorithm: HOTP (RFC 4226) with HMAC-SHA1 + a time-based counter.
//   counter = floor(now_unix_seconds / step_seconds)
//   code = HOTP(base32_secret, counter) mod 10^digits
//
// Verification allows a configurable window of ±window_steps steps to
// account for clock skew and transmission delay. Default window = 1
// (total validity window = 90 seconds for a 30-second step).
//
// Secrets are RFC 4648 base32 strings (case-insensitive; padding optional).
#ifndef FMGR_AUTH_TOTP_H
#define FMGR_AUTH_TOTP_H

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fmgr::auth {

  struct TotpConfig {
    int step_seconds{30};
    int window_steps{1}; // ±N steps are accepted; total = (2N+1)*step seconds
    int digits{6};
  };

  // Decode RFC 4648 base32 (case-insensitive, padding optional).
  // Throws std::invalid_argument on invalid characters.
  [[nodiscard]] std::vector<std::uint8_t> base32_decode(std::string_view encoded);

  // Generate a TOTP code for the given secret and time.
  // now_unix_seconds: inject a fixed value in tests; use real clock in production.
  [[nodiscard]] std::string totp_generate(std::string_view base32_secret,
                                          std::int64_t now_unix_seconds,
                                          const TotpConfig& config = {});

  // Verify a TOTP code within the configured window.
  // Returns true iff the code matches any counter in [T-window, T+window].
  [[nodiscard]] bool totp_verify(std::string_view base32_secret, std::string_view code,
                                 std::int64_t now_unix_seconds, const TotpConfig& config = {});

} // namespace fmgr::auth

#endif // FMGR_AUTH_TOTP_H
