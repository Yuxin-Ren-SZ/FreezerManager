// SPDX-License-Identifier: AGPL-3.0-or-later

// Default KMS selection, shared by the server and the freezerctl CLI so both
// resolve the master KEK the same way.
#ifndef FMGR_KMS_KMSFACTORY_H
#define FMGR_KMS_KMSFACTORY_H

#include "kms/IKmsProvider.h"

#include <memory>

namespace fmgr::kms {

  // Build the deployment's KMS provider:
  //   1. OsKeyringKms when $CREDENTIALS_DIRECTORY/master_kek exists (production);
  //   2. else EnvVarKms from FMGR_MASTER_KEK (dev/test);
  //   3. else nullptr — no key configured, so PHI storage is disabled.
  // Throws KmsError if a configured source is present but malformed (fail-fast on
  // a misconfigured key rather than silently disabling PHI).
  [[nodiscard]] std::unique_ptr<IKmsProvider> make_default_kms();

  // Build the deployment's *backup* KMS — an independent key, distinct from the
  // master KEK, so losing one cannot decrypt data sealed under the other (PRD §8,
  // §14). Resolution mirrors make_default_kms():
  //   1. OsKeyringKms when $CREDENTIALS_DIRECTORY/backup_kek exists (production);
  //   2. else EnvVarKms from FMGR_BACKUP_KEK (dev/test);
  //   3. else nullptr — no backup key configured, so encrypted backups are off.
  [[nodiscard]] std::unique_ptr<IKmsProvider> make_backup_kms();

} // namespace fmgr::kms

#endif // FMGR_KMS_KMSFACTORY_H
