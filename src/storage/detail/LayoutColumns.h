// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_STORAGE_DETAIL_LAYOUTCOLUMNS_H
#define FMGR_STORAGE_DETAIL_LAYOUTCOLUMNS_H

#include "core/freezer.h"
#include "storage/IStorageBackend.h"

#include <string>

// Backend-neutral column-name mapping and pure validation for the layout
// entities (Freezer, StorageContainer). Column names are identical across the
// SQLite and PostgreSQL schemas. The acyclic-parent invariant needs to read the
// container tree, so it stays in each backend's repository; only the cheap
// self-parent guard lives in the shared validator.
namespace fmgr::storage::detail {

  [[nodiscard]] inline std::string freezer_column_name(core::Freezer::Field field) {
    switch (field) {
    case core::Freezer::Field::Id:
      return "id";
    case core::Freezer::Field::LabId:
      return "lab_id";
    case core::Freezer::Field::Name:
      return "name";
    case core::Freezer::Field::Location:
      return "location";
    case core::Freezer::Field::Model:
      return "model";
    case core::Freezer::Field::TempTargetC:
      return "temp_target_c";
    case core::Freezer::Field::LayoutRootId:
      return "layout_root_id";
    case core::Freezer::Field::CreatedAt:
      return "created_at_micros";
    case core::Freezer::Field::ArchivedAt:
      return "archived_at_micros";
    }
    throw ConstraintViolation("unknown freezer field");
  }

  [[nodiscard]] inline std::string container_column_name(core::StorageContainer::Field field) {
    switch (field) {
    case core::StorageContainer::Field::Id:
      return "id";
    case core::StorageContainer::Field::LabId:
      return "lab_id";
    case core::StorageContainer::Field::ParentId:
      return "parent_id";
    case core::StorageContainer::Field::Kind:
      return "kind";
    case core::StorageContainer::Field::Name:
      return "name";
    case core::StorageContainer::Field::Label:
      return "label";
    case core::StorageContainer::Field::OrderingIndex:
      return "ordering_index";
    case core::StorageContainer::Field::CapacityHint:
      return "capacity_hint_json";
    case core::StorageContainer::Field::CreatedAt:
      return "created_at_micros";
    case core::StorageContainer::Field::ArchivedAt:
      return "archived_at_micros";
    }
    throw ConstraintViolation("unknown storage container field");
  }

  inline void validate_freezer(const core::Freezer& freezer) {
    if (freezer.name.empty()) {
      throw ConstraintViolation("freezer name is required");
    }
  }

  inline void validate_storage_container(const core::StorageContainer& container) {
    if (container.name.empty()) {
      throw ConstraintViolation("storage container name is required");
    }
    if (container.parent_id.has_value() && container.parent_id.value() == container.id) {
      throw ConstraintViolation("storage container cannot be its own parent");
    }
  }

} // namespace fmgr::storage::detail

#endif // FMGR_STORAGE_DETAIL_LAYOUTCOLUMNS_H
