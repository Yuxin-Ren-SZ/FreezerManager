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
#include <string_view>
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

    // Stable, non-secret identifier for the ACTIVE KEK (a fingerprint). Stored as
    // the envelope `kek_id` by wrap_dek/encrypt so a later rotation can tell which
    // KEK a record is wrapped under.
    [[nodiscard]] virtual std::string key_id() const = 0;

    // Encrypt a plaintext DEK under the active KEK.
    [[nodiscard]] virtual WrappedDek wrap_dek(std::span<const std::uint8_t> dek) const = 0;

    // Recover the plaintext DEK wrapped under the KEK named by `kek_id` (the value
    // recorded in the envelope). A provider may hold several KEKs (active +
    // retired) so records wrapped under an older KEK still decrypt during rotation.
    // Throws KmsError on an unknown kek_id or on authentication failure.
    [[nodiscard]] virtual std::vector<std::uint8_t> unwrap_dek(const WrappedDek& wrapped,
                                                               std::string_view kek_id) const = 0;
  };

} // namespace fmgr::kms

#endif // FMGR_KMS_IKMSPROVIDER_H
