// SPDX-License-Identifier: AGPL-3.0-or-later

#include "kms/KmsFactory.h"

#include "kms/EnvVarKms.h"
#include "kms/OsKeyringKms.h"

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

namespace fmgr::kms {

  namespace {

    // Shared resolver: OS keyring credential `<basename>` → env var `<env_active>`
    // (+ `<env_previous>`) → nullptr. Both make_default_kms and make_backup_kms
    // pick a different (basename, env-var) pair.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    std::unique_ptr<IKmsProvider> make_kms(const std::string& basename,
                                           const std::string& env_active,
                                           const std::string& env_previous) {
      const char* creds_dir = std::getenv("CREDENTIALS_DIRECTORY"); // NOLINT(concurrency-mt-unsafe)
      if (creds_dir != nullptr && *creds_dir != '\0') {
        std::error_code error;
        const std::filesystem::path creds_path(creds_dir);
        const std::filesystem::path active = creds_path / basename;
        if (std::filesystem::exists(active, error)) {
          // Reuse the already-resolved directory instead of re-reading
          // CREDENTIALS_DIRECTORY inside from_systemd_credentials (review F-6).
          return std::make_unique<OsKeyringKms>(
              OsKeyringKms::from_credentials_dir(creds_path, basename));
        }
      }

      const char* env_kek = std::getenv(env_active.c_str()); // NOLINT(concurrency-mt-unsafe)
      if (env_kek != nullptr && *env_kek != '\0') {
        return std::make_unique<EnvVarKms>(EnvVarKms::from_env(env_active, env_previous));
      }

      return nullptr;
    }

  } // namespace

  std::unique_ptr<IKmsProvider> make_default_kms() {
    return make_kms("master_kek", "FMGR_MASTER_KEK", "FMGR_MASTER_KEK_PREVIOUS");
  }

  std::unique_ptr<IKmsProvider> make_backup_kms() {
    return make_kms("backup_kek", "FMGR_BACKUP_KEK", "FMGR_BACKUP_KEK_PREVIOUS");
  }

} // namespace fmgr::kms
