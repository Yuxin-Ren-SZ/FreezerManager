// SPDX-License-Identifier: AGPL-3.0-or-later

// OsKeyringKms — production KMS loader that reads the master KEK (and any retired
// KEKs) from the systemd-creds credential directory, so the key never lives in
// the process environment (PRD §8).
//
// systemd places LoadCredential= secrets in a tmpfs directory named by
// $CREDENTIALS_DIRECTORY. Layout this loader expects:
//
//   <dir>/master_kek            active KEK (raw 32 bytes, or base64 of 32 bytes)
//   <dir>/master_kek.prev.<id>  retired KEKs, kept only for unwrapping older
//                               records during a rotation (any number, optional)
//
// Built on KeyringKms; pure file I/O + libsodium, no extra dependency.
#ifndef FMGR_KMS_OSKEYRINGKMS_H
#define FMGR_KMS_OSKEYRINGKMS_H

#include "kms/KeyringKms.h"

#include <filesystem>

namespace fmgr::kms {

  class OsKeyringKms final : public KeyringKms {
  public:
    // Load from an explicit credential directory. Throws KmsError if the directory
    // or the active `master_kek` file is missing, or any key is not 32 bytes.
    static OsKeyringKms from_credentials_dir(const std::filesystem::path& dir);

    // Load a named credential (active `<basename>`, retired `<basename>.prev.*`),
    // so an independent key — e.g. the backup KEK at `backup_kek` (PRD §8) — can be
    // loaded from the same directory.
    static OsKeyringKms from_credentials_dir(const std::filesystem::path& dir,
                                             const std::string& basename);

    // Load from $CREDENTIALS_DIRECTORY (set by systemd LoadCredential=). Throws
    // KmsError if the variable is unset.
    static OsKeyringKms from_systemd_credentials();

    // Load a named credential from $CREDENTIALS_DIRECTORY.
    static OsKeyringKms from_systemd_credentials(const std::string& basename);

  private:
    explicit OsKeyringKms(std::vector<std::uint8_t> active,
                          std::vector<std::vector<std::uint8_t>> retired);
  };

} // namespace fmgr::kms

#endif // FMGR_KMS_OSKEYRINGKMS_H
