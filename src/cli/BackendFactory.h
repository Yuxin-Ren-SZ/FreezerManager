// SPDX-License-Identifier: AGPL-3.0-or-later

// Compatibility wrapper for CLI code. The concrete backend factory now lives in
// storage/BackendFactory so freezerd and freezerctl share exactly the same backend
// selection, repository registration, migration, and FMGR_PG_PASSWORD handling.
#ifndef FMGR_CLI_BACKENDFACTORY_H
#define FMGR_CLI_BACKENDFACTORY_H

#include "storage/BackendFactory.h"

namespace fmgr::cli {

  using BackendOptions = storage::BackendOptions;
  using BackendOptionError = storage::BackendOptionError;
  using storage::open_backend;

} // namespace fmgr::cli

#endif // FMGR_CLI_BACKENDFACTORY_H
