// SPDX-License-Identifier: AGPL-3.0-or-later

// FileCipher — whole-file streaming envelope encryption, used for encrypted
// backups (PRD §14). Where FieldCipher seals small per-field values in memory,
// this streams an arbitrary-size file (a hot-copied SQLite database) so memory
// stays bounded regardless of database size.
//
// A fresh per-archive DEK is generated, the file is encrypted with libsodium's
// crypto_secretstream (chunked, each chunk AEAD-authenticated), and the DEK is
// wrapped by the injected IKmsProvider — which for backups is the *separate*
// backup KEK (kms::make_backup_kms), so losing the live master key alone cannot
// decrypt a backup (PRD §8).
//
// On-disk format:
//   line 1: a single-line JSON manifest, terminated by '\n':
//     { "magic":"FMGRBAK", "v":1, "kek_id":"...", "dek":{"n":b64,"c":b64},
//       "ss_header":b64, "schema_version":N, "backend":"sqlite",
//       "created_at_micros":N, "content_sha256":"<hex>" }
//   then: a sequence of length-prefixed ciphertext chunks (uint32 little-endian
//     length, then that many ciphertext bytes); the final chunk carries the
//     secretstream FINAL tag.
//
// `content_sha256` is the SHA-256 of the *plaintext*, giving an integrity check
// independent of the per-chunk AEAD tags; decrypt_file verifies it and throws on
// mismatch.
#ifndef FMGR_CRYPTO_FILECIPHER_H
#define FMGR_CRYPTO_FILECIPHER_H

#include "crypto/FieldCipher.h" // for crypto::CipherError
#include "kms/IKmsProvider.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace fmgr::crypto {

  // Manifest metadata recorded in (encrypt) / recovered from (decrypt) a backup.
  struct BackupManifest {
    int version{};
    std::string kek_id;
    int schema_version{};
    std::string backend;
    std::int64_t created_at_micros{};
    std::string content_sha256; // hex of the plaintext SHA-256
  };

  // Caller-supplied metadata stamped into the manifest at encrypt time.
  struct FileEnvelopeMeta {
    int schema_version{};
    std::string backend;
    std::int64_t created_at_micros{};
  };

  // Encrypt `in_path` to `out_path` under a fresh DEK wrapped by `kms`. Returns
  // the manifest (including the computed content hash). Throws CipherError if a
  // file cannot be opened.
  BackupManifest encrypt_file(const std::filesystem::path& in_path,
                              const std::filesystem::path& out_path, const kms::IKmsProvider& kms,
                              const FileEnvelopeMeta& meta);

  // Decrypt `in_path` to `out_path`, authenticating every chunk and verifying the
  // plaintext content hash. Returns the manifest. Throws CipherError on a
  // malformed manifest, wrong key, tampered ciphertext, or content-hash mismatch.
  BackupManifest decrypt_file(const std::filesystem::path& in_path,
                              const std::filesystem::path& out_path, const kms::IKmsProvider& kms);

} // namespace fmgr::crypto

#endif // FMGR_CRYPTO_FILECIPHER_H
