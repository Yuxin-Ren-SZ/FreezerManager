// SPDX-License-Identifier: AGPL-3.0-or-later

// EnvVarKms — development/test KMS loader that holds the master KEK in process
// memory, sourced from environment variables and built on KeyringKms.
//
// NOT FOR PRODUCTION: the KEK is recoverable from the process environment and a
// core dump. Production deployments must use OsKeyringKms (systemd-creds) or a
// future VaultKms. Documented in PRD §8.
//
//   FMGR_MASTER_KEK           base64 of the active 32-byte KEK (required)
//   FMGR_MASTER_KEK_PREVIOUS  comma-separated base64 of retired KEKs (optional),
//                             kept only so records still wrapped under them can be
//                             decrypted during a rotation.
#ifndef FMGR_KMS_ENVVARKMS_H
#define FMGR_KMS_ENVVARKMS_H

#include "kms/KeyringKms.h"

#include <cstdint>
#include <string>
#include <vector>

namespace fmgr::kms {

  class EnvVarKms final : public KeyringKms {
  public:
    // Read FMGR_MASTER_KEK (+ optional FMGR_MASTER_KEK_PREVIOUS). Throws KmsError
    // if the active variable is unset or any value is not base64 of a 32-byte key.
    static EnvVarKms from_env();

    // Read an arbitrary active/previous variable pair, so an independent key (e.g.
    // the backup KEK from FMGR_BACKUP_KEK / FMGR_BACKUP_KEK_PREVIOUS, PRD §8) can
    // be loaded the same way. `previous_var` may be empty to skip retired keys.
    static EnvVarKms from_env(const std::string& active_var, const std::string& previous_var);

    // Decode a single base64 active KEK (no retired keys). Used by tests.
    static EnvVarKms from_base64(const std::string& kek_base64);

    // Construct from raw KEK material (active + optional retired). Used by tests.
    explicit EnvVarKms(std::vector<std::uint8_t> active,
                       std::vector<std::vector<std::uint8_t>> retired = {});
  };

} // namespace fmgr::kms

#endif // FMGR_KMS_ENVVARKMS_H
