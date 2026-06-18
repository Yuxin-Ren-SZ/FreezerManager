// SPDX-License-Identifier: AGPL-3.0-or-later

#include "kms/EnvVarKms.h"

#include <sodium.h>

#include <array>
#include <cstdlib>
#include <string>

namespace fmgr::kms {

  namespace {

    void ensure_sodium() {
      if (sodium_init() < 0) {
        throw KmsError("libsodium initialization failed");
      }
    }

    // Short, non-secret KEK fingerprint: hex of a keyed-less BLAKE2b digest,
    // truncated. Lets wrapped DEKs record which KEK produced them without
    // exposing key material.
    std::string fingerprint(const std::vector<std::uint8_t>& kek) {
      std::array<unsigned char, crypto_generichash_BYTES> hash{};
      crypto_generichash(hash.data(), hash.size(), kek.data(), kek.size(), nullptr, 0);
      static constexpr std::array<char, 16> k_hex{'0', '1', '2', '3', '4', '5', '6', '7',
                                                  '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
      std::string out;
      out.reserve(16);
      for (std::size_t i = 0; i < 8; ++i) {
        out.push_back(k_hex[static_cast<std::size_t>(hash[i] >> 4)]);
        out.push_back(k_hex[static_cast<std::size_t>(hash[i] & 0x0F)]);
      }
      return out;
    }

  } // namespace

  EnvVarKms::EnvVarKms(std::vector<std::uint8_t> kek) : kek_(std::move(kek)) {
    ensure_sodium();
    if (kek_.size() != crypto_secretbox_KEYBYTES) {
      throw KmsError("master KEK must be exactly 32 bytes");
    }
    key_id_ = fingerprint(kek_);
  }

  EnvVarKms EnvVarKms::from_base64(const std::string& kek_base64) {
    ensure_sodium();
    std::vector<std::uint8_t> kek(crypto_secretbox_KEYBYTES);
    std::size_t decoded_len = 0;
    if (sodium_base642bin(kek.data(), kek.size(), kek_base64.data(), kek_base64.size(), nullptr,
                          &decoded_len, nullptr, sodium_base64_VARIANT_ORIGINAL) != 0) {
      throw KmsError("master KEK is not valid base64 of a 32-byte key");
    }
    if (decoded_len != crypto_secretbox_KEYBYTES) {
      throw KmsError("master KEK must decode to exactly 32 bytes");
    }
    return EnvVarKms(std::move(kek));
  }

  EnvVarKms EnvVarKms::from_env() {
    const char* raw = std::getenv("FMGR_MASTER_KEK"); // NOLINT(concurrency-mt-unsafe)
    if (raw == nullptr || *raw == '\0') {
      throw KmsError("FMGR_MASTER_KEK is not set");
    }
    return from_base64(std::string(raw));
  }

  std::string EnvVarKms::key_id() const {
    return key_id_;
  }

  WrappedDek EnvVarKms::wrap_dek(std::span<const std::uint8_t> dek) const {
    WrappedDek wrapped;
    wrapped.nonce.resize(crypto_secretbox_NONCEBYTES);
    randombytes_buf(wrapped.nonce.data(), wrapped.nonce.size());
    wrapped.ciphertext.resize(dek.size() + crypto_secretbox_MACBYTES);
    crypto_secretbox_easy(wrapped.ciphertext.data(), dek.data(), dek.size(), wrapped.nonce.data(),
                          kek_.data());
    return wrapped;
  }

  std::vector<std::uint8_t> EnvVarKms::unwrap_dek(const WrappedDek& wrapped) const {
    if (wrapped.nonce.size() != crypto_secretbox_NONCEBYTES ||
        wrapped.ciphertext.size() < crypto_secretbox_MACBYTES) {
      throw KmsError("wrapped DEK is malformed");
    }
    std::vector<std::uint8_t> dek(wrapped.ciphertext.size() - crypto_secretbox_MACBYTES);
    if (crypto_secretbox_open_easy(dek.data(), wrapped.ciphertext.data(), wrapped.ciphertext.size(),
                                   wrapped.nonce.data(), kek_.data()) != 0) {
      throw KmsError("wrapped DEK failed authentication");
    }
    return dek;
  }

} // namespace fmgr::kms
