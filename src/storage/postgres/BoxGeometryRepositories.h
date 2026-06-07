// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_STORAGE_POSTGRES_BOXGEOMETRYREPOSITORIES_H
#define FMGR_STORAGE_POSTGRES_BOXGEOMETRYREPOSITORIES_H

#include "storage/postgres/PostgresBackend.h"

namespace fmgr::storage {

  void register_box_geometry_repositories(PostgresBackend& backend);
  void register_box_repositories(PostgresBackend& backend);

} // namespace fmgr::storage

#endif // FMGR_STORAGE_POSTGRES_BOXGEOMETRYREPOSITORIES_H
