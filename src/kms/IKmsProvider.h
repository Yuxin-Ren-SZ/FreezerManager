// SPDX-License-Identifier: AGPL-3.0-or-later

// Pluggable key-management provider (PRD §8).
//
// PHI field encryption uses envelope encryption: each record is encrypted under
// a fresh per-record data-encryption key (DEK), and the DEK is *wrapped*
// (encrypted) under a long-lived master key-encryption key (KEK) that lives
// outside the database. The KEK never leaves the provider; callers only ever
// hold plaintext DEKs (transiently, in memory) and wrapped DEKs (at rest).
//
// v1 reference implementation: EnvVarKms (dev/test). OsKeyringKms / VaultKms are
// later slices that satisfy the same interface.
#ifndef FMGR_KMS_IKMSPROVIDER_H
#define FMGR_KMS_IKMSPROVIDER_H

#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace fmgr::kms {

  // Thrown on any KMS failure: missing/short key material, or a wrapped DEK that
  // fails authentication (tamper, wrong key). Messages never contain key bytes.
  class KmsError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
  };

  // A DEK encrypted under the master KEK. Both fields are non-secret at rest;
  // the ciphertext is AEAD-authenticated so tampering is detected on unwrap.
  struct WrappedDek {
    std::vector<std::uint8_t> nonce;
    std::vector<std::uint8_t> ciphertext;

    friend bool operator==(const WrappedDek&, const WrappedDek&) = default;
  };

  class IKmsProvider {
  public:
    virtual ~IKmsProvider() = default;

    // Stable, non-secret identifier for the active KEK (e.g. a fingerprint).
    // Stored alongside wrapped DEKs so a future rotation can select the KEK.
    [[nodiscard]] virtual std::string key_id() const = 0;

    // Encrypt a plaintext DEK under the master KEK.
    [[nodiscard]] virtual WrappedDek wrap_dek(std::span<const std::uint8_t> dek) const = 0;

    // Recover the plaintext DEK. Throws KmsError if authentication fails.
    [[nodiscard]] virtual std::vector<std::uint8_t> unwrap_dek(const WrappedDek& wrapped) const = 0;
  };

} // namespace fmgr::kms

#endif // FMGR_KMS_IKMSPROVIDER_H
