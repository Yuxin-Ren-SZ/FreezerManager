// SPDX-License-Identifier: AGPL-3.0-or-later

#include "auth/Totp.h"

#include <openssl/evp.h>
#include <openssl/params.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

namespace fmgr::auth {
  namespace {

    // RFC 4648 base32 alphabet (upper-case).
    constexpr std::string_view k_base32_alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

    // HMAC-SHA1 via OpenSSL 3 EVP_MAC API (avoids the deprecated HMAC() function).
    // Returns the 20-byte HMAC-SHA1 digest.
    //
    // 'counter' and 'key' are adjacent int64 + vector parameters that are not
    // swappable (different types), but clang-tidy flags the function signature anyway.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    [[nodiscard]] std::array<unsigned char, 20> hmac_sha1(const std::vector<std::uint8_t>& key,
                                                          std::int64_t counter_val) {
      // counter is 8 bytes big-endian (RFC 4226 §5.2)
      std::array<unsigned char, 8> counter_bytes{};
      auto counter_be = static_cast<std::uint64_t>(counter_val);
      for (int i = 7; i >= 0; --i) {
        counter_bytes.at(static_cast<std::size_t>(i)) =
            static_cast<unsigned char>(counter_be & 0xFFU);
        counter_be >>= 8U;
      }

      EVP_MAC* mac = EVP_MAC_fetch(nullptr, "HMAC", nullptr);
      if (mac == nullptr) {
        throw std::runtime_error("failed to fetch HMAC algorithm");
      }

      EVP_MAC_CTX* ctx = EVP_MAC_CTX_new(mac);
      EVP_MAC_free(mac);
      if (ctx == nullptr) {
        throw std::runtime_error("failed to create HMAC context");
      }

      // NOLINTNEXTLINE(modernize-avoid-c-arrays) — required by the OpenSSL C API
      char sha1_name[] = "SHA1";
      // NOLINTNEXTLINE(modernize-avoid-c-arrays) — OSSL_PARAM is a C struct array
      OSSL_PARAM params[] = {
          OSSL_PARAM_construct_utf8_string("digest", sha1_name, 0),
          OSSL_PARAM_construct_end(),
      };

      if (EVP_MAC_init(ctx, key.data(), key.size(), params) != 1) {
        EVP_MAC_CTX_free(ctx);
        throw std::runtime_error("failed to initialize HMAC");
      }
      if (EVP_MAC_update(ctx, counter_bytes.data(), counter_bytes.size()) != 1) {
        EVP_MAC_CTX_free(ctx);
        throw std::runtime_error("failed to update HMAC");
      }

      std::array<unsigned char, 20> result{};
      std::size_t out_len = result.size();
      if (EVP_MAC_final(ctx, result.data(), &out_len, result.size()) != 1) {
        EVP_MAC_CTX_free(ctx);
        throw std::runtime_error("failed to finalize HMAC");
      }
      EVP_MAC_CTX_free(ctx);
      return result;
    }

    // RFC 4226 §5.3 dynamic truncation.
    [[nodiscard]] std::uint32_t hotp_truncate(const std::array<unsigned char, 20>& hmac) {
      const std::size_t offset = hmac.at(19) & 0x0FU;
      const std::uint32_t value = (static_cast<std::uint32_t>(hmac.at(offset) & 0x7FU) << 24U) |
                                  (static_cast<std::uint32_t>(hmac.at(offset + 1U)) << 16U) |
                                  (static_cast<std::uint32_t>(hmac.at(offset + 2U)) << 8U) |
                                  static_cast<std::uint32_t>(hmac.at(offset + 3U));
      return value;
    }

    // Compute HOTP code for the given key bytes and counter.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    [[nodiscard]] std::string hotp(const std::vector<std::uint8_t>& key, std::int64_t counter,
                                   int digits) {
      const auto hmac = hmac_sha1(key, counter);
      const std::uint32_t truncated = hotp_truncate(hmac);

      std::uint32_t modulus = 1U;
      for (int i = 0; i < digits; ++i) {
        modulus *= 10U;
      }
      const std::uint32_t otp = truncated % modulus;

      std::string code = std::to_string(otp);
      // Left-pad with zeros to exactly `digits` characters.
      while (static_cast<std::size_t>(code.size()) < static_cast<std::size_t>(digits)) {
        code.insert(code.begin(), '0');
      }
      return code;
    }

  } // namespace

  std::vector<std::uint8_t> base32_decode(std::string_view encoded) {
    std::vector<std::uint8_t> output;
    output.reserve(encoded.size() * 5 / 8);

    std::uint32_t buffer = 0;
    int bits_left = 0;

    for (const char raw_ch : encoded) {
      // Strip padding.
      if (raw_ch == '=') {
        break;
      }

      const char upper_ch = static_cast<char>(::toupper(static_cast<unsigned char>(raw_ch)));
      const auto pos = k_base32_alphabet.find(upper_ch);
      if (pos == std::string_view::npos) {
        throw std::invalid_argument(std::string("invalid base32 character: ") + raw_ch);
      }

      buffer = (buffer << 5U) | static_cast<std::uint32_t>(pos);
      bits_left += 5;

      if (bits_left >= 8) {
        bits_left -= 8;
        output.push_back(
            static_cast<std::uint8_t>((buffer >> static_cast<unsigned>(bits_left)) & 0xFFU));
      }
    }

    return output;
  }

  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  std::string totp_generate(std::string_view base32_secret, std::int64_t now_unix_seconds,
                            const TotpConfig& config) {
    const auto key = base32_decode(base32_secret);
    const std::int64_t counter = now_unix_seconds / config.step_seconds;
    return hotp(key, counter, config.digits);
  }

  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  bool totp_verify(std::string_view base32_secret, std::string_view code,
                   std::int64_t now_unix_seconds, const TotpConfig& config) {
    const auto key = base32_decode(base32_secret);
    const std::int64_t base_counter = now_unix_seconds / config.step_seconds;

    for (int delta = -config.window_steps; delta <= config.window_steps; ++delta) {
      const std::int64_t counter = base_counter + delta;
      if (counter < 0) {
        continue;
      }
      if (hotp(key, counter, config.digits) == code) {
        return true;
      }
    }
    return false;
  }

} // namespace fmgr::auth
