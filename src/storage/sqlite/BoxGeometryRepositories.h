// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_STORAGE_SQLITE_BOXGEOMETRYREPOSITORIES_H
#define FMGR_STORAGE_SQLITE_BOXGEOMETRYREPOSITORIES_H

#include "storage/sqlite/SqliteBackend.h"

namespace fmgr::storage {

  void register_box_geometry_repositories(SqliteBackend& backend);
  void register_box_repositories(SqliteBackend& backend);

} // namespace fmgr::storage

#endif // FMGR_STORAGE_SQLITE_BOXGEOMETRYREPOSITORIES_H
