// SPDX-License-Identifier: AGPL-3.0-or-later

// Shared value type for build-time-embedded schema migrations. The per-backend
// embedded arrays (SqliteMigrationsEmbedded.h / PostgresMigrationsEmbedded.h,
// generated from the migrations/*.sql files) are std::array<EmbeddedMigration>.
#ifndef FMGR_STORAGE_EMBEDDEDMIGRATION_H
#define FMGR_STORAGE_EMBEDDEDMIGRATION_H

#include <string_view>

namespace fmgr::storage {

  struct EmbeddedMigration {
    int version;
    std::string_view name;
    std::string_view up_sql;
  };

} // namespace fmgr::storage

#endif // FMGR_STORAGE_EMBEDDEDMIGRATION_H
