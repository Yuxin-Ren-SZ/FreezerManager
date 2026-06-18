// SPDX-License-Identifier: AGPL-3.0-or-later

#include "kms/KmsFactory.h"

#include "kms/EnvVarKms.h"
#include "kms/OsKeyringKms.h"

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

namespace fmgr::kms {

  std::unique_ptr<IKmsProvider> make_default_kms() {
    const char* creds_dir = std::getenv("CREDENTIALS_DIRECTORY"); // NOLINT(concurrency-mt-unsafe)
    if (creds_dir != nullptr && *creds_dir != '\0') {
      std::error_code error;
      const std::filesystem::path active = std::filesystem::path(creds_dir) / "master_kek";
      if (std::filesystem::exists(active, error)) {
        return std::make_unique<OsKeyringKms>(OsKeyringKms::from_systemd_credentials());
      }
    }

    const char* env_kek = std::getenv("FMGR_MASTER_KEK"); // NOLINT(concurrency-mt-unsafe)
    if (env_kek != nullptr && *env_kek != '\0') {
      return std::make_unique<EnvVarKms>(EnvVarKms::from_env());
    }

    return nullptr;
  }

} // namespace fmgr::kms
