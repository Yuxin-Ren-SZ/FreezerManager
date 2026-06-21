// SPDX-License-Identifier: AGPL-3.0-or-later

#include "kms/KeyringKms.h"

#include "core/sodium_init.h"

#include <sodium.h>

#include <array>
#include <utility>

namespace fmgr::kms {

  namespace {

    void ensure_sodium() {
      if (!core::sodium_ready()) {
        throw KmsError("libsodium initialization failed");
      }
    }

    void require_kek_size(const std::vector<std::uint8_t>& kek) {
      if (kek.size() != crypto_secretbox_KEYBYTES) {
        throw KmsError("KEK must be exactly 32 bytes");
      }
    }

  } // namespace

  std::string KeyringKms::fingerprint(std::span<const std::uint8_t> kek) {
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

  KeyringKms::KeyringKms(std::vector<std::uint8_t> active,
                         std::vector<std::vector<std::uint8_t>> retired) {
    ensure_sodium();
    require_kek_size(active);
    active_id_ = fingerprint(active);
    keks_.emplace(active_id_, std::move(active));
    for (auto& kek : retired) {
      require_kek_size(kek);
      keks_.insert_or_assign(fingerprint(kek), std::move(kek));
    }
  }

  std::string KeyringKms::key_id() const {
    return active_id_;
  }

  WrappedDek KeyringKms::wrap_dek(std::span<const std::uint8_t> dek) const {
    const auto& kek = keks_.at(active_id_);
    WrappedDek wrapped;
    wrapped.nonce.resize(crypto_secretbox_NONCEBYTES);
    randombytes_buf(wrapped.nonce.data(), wrapped.nonce.size());
    wrapped.ciphertext.resize(dek.size() + crypto_secretbox_MACBYTES);
    crypto_secretbox_easy(wrapped.ciphertext.data(), dek.data(), dek.size(), wrapped.nonce.data(),
                          kek.data());
    return wrapped;
  }

  std::vector<std::uint8_t> KeyringKms::unwrap_dek(const WrappedDek& wrapped,
                                                   std::string_view kek_id) const {
    const auto found = keks_.find(std::string(kek_id));
    if (found == keks_.end()) {
      throw KmsError("no KEK in the keyring matches the record's kek_id");
    }
    if (wrapped.nonce.size() != crypto_secretbox_NONCEBYTES ||
        wrapped.ciphertext.size() < crypto_secretbox_MACBYTES) {
      throw KmsError("wrapped DEK is malformed");
    }
    std::vector<std::uint8_t> dek(wrapped.ciphertext.size() - crypto_secretbox_MACBYTES);
    if (crypto_secretbox_open_easy(dek.data(), wrapped.ciphertext.data(), wrapped.ciphertext.size(),
                                   wrapped.nonce.data(), found->second.data()) != 0) {
      throw KmsError("wrapped DEK failed authentication");
    }
    return dek;
  }

} // namespace fmgr::kms
