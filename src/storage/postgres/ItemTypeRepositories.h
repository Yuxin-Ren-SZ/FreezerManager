// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_STORAGE_POSTGRES_ITEMTYPEREPOSITORIES_H
#define FMGR_STORAGE_POSTGRES_ITEMTYPEREPOSITORIES_H

#include "storage/postgres/PostgresBackend.h"

namespace fmgr::storage {

  void register_item_type_repositories(PostgresBackend& backend);

} // namespace fmgr::storage

#endif // FMGR_STORAGE_POSTGRES_ITEMTYPEREPOSITORIES_H
