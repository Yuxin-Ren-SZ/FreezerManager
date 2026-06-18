// SPDX-License-Identifier: AGPL-3.0-or-later

// EnvVarKms — development/test KMS that holds the master KEK in process memory,
// sourced from the FMGR_MASTER_KEK environment variable (base64, 32 bytes).
//
// NOT FOR PRODUCTION: the KEK is recoverable from the process environment and a
// core dump. Production deployments must use OsKeyringKms / VaultKms (later
// slices), which satisfy the same IKmsProvider interface. Documented in PRD §8.
//
// DEK wrapping uses libsodium crypto_secretbox (XSalsa20-Poly1305) with a random
// per-wrap nonce.
#ifndef FMGR_KMS_ENVVARKMS_H
#define FMGR_KMS_ENVVARKMS_H

#include "kms/IKmsProvider.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace fmgr::kms {

  class EnvVarKms final : public IKmsProvider {
  public:
    // Reads FMGR_MASTER_KEK (base64 of exactly 32 bytes). Throws KmsError if the
    // variable is unset, not valid base64, or the wrong length.
    static EnvVarKms from_env();

    // Decode a base64 KEK directly (used by from_env and by tests).
    static EnvVarKms from_base64(const std::string& kek_base64);

    // Construct from raw 32-byte KEK material (used by tests). Throws on wrong size.
    explicit EnvVarKms(std::vector<std::uint8_t> kek);

    [[nodiscard]] std::string key_id() const override;
    [[nodiscard]] WrappedDek wrap_dek(std::span<const std::uint8_t> dek) const override;
    [[nodiscard]] std::vector<std::uint8_t> unwrap_dek(const WrappedDek& wrapped) const override;

  private:
    std::vector<std::uint8_t> kek_;
    std::string key_id_;
  };

} // namespace fmgr::kms

#endif // FMGR_KMS_ENVVARKMS_H
