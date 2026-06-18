// SPDX-License-Identifier: AGPL-3.0-or-later

// FieldCipher — per-record envelope encryption for PHI custom fields (PRD §8).
//
// encrypt() takes a map of PHI field key -> JSON value and produces the
// `phi_fields_enc_json` envelope string stored on Sample. A fresh DEK is
// generated per call (per sample-write), each field value is sealed under that
// DEK with crypto_secretbox, and the DEK is wrapped by the injected
// IKmsProvider. decrypt() reverses it, authenticating every field; any tamper
// throws CipherError.
//
// Envelope JSON (version 1):
//   { "v": 1,
//     "kek_id": "<provider key id>",
//     "dek": { "n": "<b64 nonce>", "c": "<b64 wrapped-dek ciphertext>" },
//     "fields": { "<key>": { "n": "<b64 nonce>", "c": "<b64 ciphertext>" } } }
//
// An empty input map serializes to "{}" (no DEK generated, no KMS call), so a
// sample with no PHI fields keeps its default empty blob.
#ifndef FMGR_CRYPTO_FIELDCIPHER_H
#define FMGR_CRYPTO_FIELDCIPHER_H

#include "kms/IKmsProvider.h"

#include <nlohmann/json.hpp>

#include <map>
#include <optional>
#include <stdexcept>
#include <string>

namespace fmgr::crypto {

  class CipherError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
  };

  // Map of PHI field key -> decrypted JSON value. Ordered for deterministic
  // envelope field ordering (aids reproducibility and review).
  using PhiFields = std::map<std::string, nlohmann::json>;

  // Encrypt PHI fields into the `phi_fields_enc_json` envelope string.
  // Returns "{}" when `fields` is empty.
  [[nodiscard]] std::string encrypt(const PhiFields& fields, const kms::IKmsProvider& kms);

  // Decrypt an envelope produced by encrypt(). Returns an empty map for "{}" or
  // an empty/whitespace string. Throws CipherError on malformed envelope or
  // authentication failure.
  [[nodiscard]] PhiFields decrypt(const std::string& envelope_json, const kms::IKmsProvider& kms);

  // Re-wrap an envelope's per-record DEK under the KMS's *active* KEK, for key
  // rotation. Field ciphertext is untouched (the DEK is unchanged; only its KEK
  // wrapping and the recorded kek_id change), so no plaintext PHI is exposed.
  // Returns nullopt when there is nothing to do: an empty/"{}" envelope, or one
  // already wrapped under the active KEK. Throws CipherError/KmsError if the
  // envelope is malformed or its current KEK is unavailable in the keyring.
  [[nodiscard]] std::optional<std::string> rewrap(const std::string& envelope_json,
                                                  const kms::IKmsProvider& kms);

} // namespace fmgr::crypto

#endif // FMGR_CRYPTO_FIELDCIPHER_H
