// SPDX-License-Identifier: AGPL-3.0-or-later

#include "kms/EnvVarKms.h"

#include <sodium.h>

#include <cstdlib>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace fmgr::kms {

  namespace {

    void ensure_sodium() {
      if (sodium_init() < 0) {
        throw KmsError("libsodium initialization failed");
      }
    }

    std::vector<std::uint8_t> decode_kek_base64(const std::string& text) {
      std::vector<std::uint8_t> kek(crypto_secretbox_KEYBYTES);
      std::size_t decoded_len = 0;
      if (sodium_base642bin(kek.data(), kek.size(), text.data(), text.size(), nullptr, &decoded_len,
                            nullptr, sodium_base64_VARIANT_ORIGINAL) != 0) {
        throw KmsError("KEK is not valid base64 of a 32-byte key");
      }
      if (decoded_len != crypto_secretbox_KEYBYTES) {
        throw KmsError("KEK must decode to exactly 32 bytes");
      }
      return kek;
    }

  } // namespace

  EnvVarKms::EnvVarKms(std::vector<std::uint8_t> active,
                       std::vector<std::vector<std::uint8_t>> retired)
      : KeyringKms(std::move(active), std::move(retired)) {}

  EnvVarKms EnvVarKms::from_base64(const std::string& kek_base64) {
    ensure_sodium();
    return EnvVarKms(decode_kek_base64(kek_base64));
  }

  EnvVarKms EnvVarKms::from_env() {
    return from_env("FMGR_MASTER_KEK", "FMGR_MASTER_KEK_PREVIOUS");
  }

  EnvVarKms EnvVarKms::from_env(const std::string& active_var, const std::string& previous_var) {
    ensure_sodium();
    const char* active = std::getenv(active_var.c_str()); // NOLINT(concurrency-mt-unsafe)
    if (active == nullptr || *active == '\0') {
      throw KmsError(active_var + " is not set");
    }

    std::vector<std::vector<std::uint8_t>> retired;
    if (!previous_var.empty()) {
      const char* previous = std::getenv(previous_var.c_str()); // NOLINT(concurrency-mt-unsafe)
      if (previous != nullptr && *previous != '\0') {
        std::istringstream stream{std::string(previous)};
        std::string item;
        while (std::getline(stream, item, ',')) {
          if (!item.empty()) {
            retired.push_back(decode_kek_base64(item));
          }
        }
      }
    }
    return EnvVarKms(decode_kek_base64(std::string(active)), std::move(retired));
  }

} // namespace fmgr::kms
