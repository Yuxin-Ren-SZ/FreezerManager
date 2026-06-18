// SPDX-License-Identifier: AGPL-3.0-or-later

// KeyringKms — in-memory KMS holding one or more 32-byte KEKs keyed by a
// non-secret fingerprint, with one designated *active* KEK.
//
// This is the shared engine for every key source: a concrete provider (EnvVarKms,
// OsKeyringKms) is just a loader that gathers raw key bytes from somewhere and
// constructs a KeyringKms. DEK wrapping uses libsodium crypto_secretbox under the
// active KEK; unwrap selects the KEK named by the envelope's kek_id, so records
// wrapped under a now-retired KEK still decrypt while a rotation is in flight.
#ifndef FMGR_KMS_KEYRINGKMS_H
#define FMGR_KMS_KEYRINGKMS_H

#include "kms/IKmsProvider.h"

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fmgr::kms {

  class KeyringKms : public IKmsProvider {
  public:
    // `active` is the KEK new DEKs are wrapped under; `retired` KEKs are kept only
    // for unwrapping older records. Every key must be exactly 32 bytes. Throws
    // KmsError otherwise. A retired key equal to the active key is harmless.
    explicit KeyringKms(std::vector<std::uint8_t> active,
                        std::vector<std::vector<std::uint8_t>> retired = {});

    // Non-secret fingerprint of a KEK (truncated BLAKE2b hex). Exposed so loaders
    // and tests can predict the id a key will be registered under.
    [[nodiscard]] static std::string fingerprint(std::span<const std::uint8_t> kek);

    [[nodiscard]] std::string key_id() const override;
    [[nodiscard]] WrappedDek wrap_dek(std::span<const std::uint8_t> dek) const override;
    [[nodiscard]] std::vector<std::uint8_t> unwrap_dek(const WrappedDek& wrapped,
                                                       std::string_view kek_id) const override;

  private:
    std::map<std::string, std::vector<std::uint8_t>> keks_; // fingerprint -> key bytes
    std::string active_id_;
  };

} // namespace fmgr::kms

#endif // FMGR_KMS_KEYRINGKMS_H
