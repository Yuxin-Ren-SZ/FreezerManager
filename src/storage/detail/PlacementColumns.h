// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_STORAGE_DETAIL_PLACEMENTCOLUMNS_H
#define FMGR_STORAGE_DETAIL_PLACEMENTCOLUMNS_H

#include "core/placement.h"
#include "storage/IStorageBackend.h"

#include <optional>

// Backend-neutral validation of the 3D layout columns (ADR 0003, migration 15).
// The schema CHECKs enforce the same rules; validating first gives the caller a
// portable ConstraintViolation before any SQL runs. Whether a placement fits its
// parent's grid needs the parent and the freezer model, so that check belongs to
// the service layer, not to the repositories.
namespace fmgr::storage::detail {

  inline void validate_placement_column(const std::optional<core::Placement>& placement) {
    if (!placement.has_value()) {
      return;
    }
    if (placement->level < 0 || placement->column < 0 || placement->depth < 0) {
      throw ConstraintViolation("placement coordinates must not be negative");
    }
  }

  inline void validate_grid_column(const std::optional<core::GridSpec>& grid) {
    if (!grid.has_value()) {
      return;
    }
    if (const auto error = core::grid_error(grid.value()); error.has_value()) {
      throw ConstraintViolation(error.value());
    }
  }

} // namespace fmgr::storage::detail

#endif // FMGR_STORAGE_DETAIL_PLACEMENTCOLUMNS_H
