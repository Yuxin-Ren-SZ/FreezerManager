// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_STORAGE_POSTGRES_SHAREREQUESTREPOSITORIES_H
#define FMGR_STORAGE_POSTGRES_SHAREREQUESTREPOSITORIES_H

#include "storage/postgres/PostgresBackend.h"

namespace fmgr::storage {

  void register_share_request_repositories(PostgresBackend& backend);

} // namespace fmgr::storage

#endif // FMGR_STORAGE_POSTGRES_SHAREREQUESTREPOSITORIES_H
