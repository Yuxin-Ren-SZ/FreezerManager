// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_STORAGE_SQLITE_ROLEREPOSITORIES_H
#define FMGR_STORAGE_SQLITE_ROLEREPOSITORIES_H

#include "storage/sqlite/SqliteBackend.h"

namespace fmgr::storage {

  void register_role_repositories(SqliteBackend& backend);

} // namespace fmgr::storage

#endif // FMGR_STORAGE_SQLITE_ROLEREPOSITORIES_H
