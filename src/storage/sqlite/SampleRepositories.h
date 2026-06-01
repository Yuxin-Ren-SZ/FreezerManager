// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_STORAGE_SQLITE_SAMPLEREPOSITORIES_H
#define FMGR_STORAGE_SQLITE_SAMPLEREPOSITORIES_H

#include "storage/sqlite/SqliteBackend.h"

namespace fmgr::storage {

  // Registers typed SQLite repositories for Sample, Project, SampleProject,
  // and CheckoutEvent with the given backend.
  void register_sample_repositories(SqliteBackend& backend);

} // namespace fmgr::storage

#endif // FMGR_STORAGE_SQLITE_SAMPLEREPOSITORIES_H
