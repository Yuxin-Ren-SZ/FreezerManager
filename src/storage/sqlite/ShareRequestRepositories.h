// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_STORAGE_SQLITE_SHAREREQUESTREPOSITORIES_H
#define FMGR_STORAGE_SQLITE_SHAREREQUESTREPOSITORIES_H

#include "storage/sqlite/SqliteBackend.h"

namespace fmgr::storage {

  void register_share_request_repositories(SqliteBackend& backend);

} // namespace fmgr::storage

#endif // FMGR_STORAGE_SQLITE_SHAREREQUESTREPOSITORIES_H
