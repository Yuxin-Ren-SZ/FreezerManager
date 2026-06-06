// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_STORAGE_DETAIL_ITEMTYPECOLUMNS_H
#define FMGR_STORAGE_DETAIL_ITEMTYPECOLUMNS_H

#include "core/item_type.h"
#include "storage/IStorageBackend.h"

#include <string>

// Backend-neutral column-name mapping and pure validation for ItemType and
// CustomFieldDefinition. Column names are identical across both schemas. The
// item-type acyclic invariant and the CFD item_type liveness check need to read
// the tables, so they stay in each backend's repository; only the shape checks
// are shared here.
namespace fmgr::storage::detail {

  [[nodiscard]] inline std::string item_type_column_name(core::ItemType::Field field) {
    switch (field) {
    case core::ItemType::Field::Id:
      return "id";
    case core::ItemType::Field::LabId:
      return "lab_id";
    case core::ItemType::Field::ParentId:
      return "parent_id";
    case core::ItemType::Field::Name:
      return "name";
    case core::ItemType::Field::CreatedAt:
      return "created_at_micros";
    case core::ItemType::Field::ArchivedAt:
      return "archived_at_micros";
    }
    throw ConstraintViolation("unknown item type field");
  }

  [[nodiscard]] inline std::string cfd_column_name(core::CustomFieldDefinition::Field field) {
    switch (field) {
    case core::CustomFieldDefinition::Field::Id:
      return "id";
    case core::CustomFieldDefinition::Field::LabId:
      return "lab_id";
    case core::CustomFieldDefinition::Field::ScopeKind:
      return "scope_kind";
    case core::CustomFieldDefinition::Field::ItemTypeId:
      return "item_type_id";
    case core::CustomFieldDefinition::Field::Key:
      return "key";
    case core::CustomFieldDefinition::Field::Label:
      return "label";
    case core::CustomFieldDefinition::Field::DataType:
      return "data_type";
    case core::CustomFieldDefinition::Field::Required:
      return "required";
    case core::CustomFieldDefinition::Field::ValidationJson:
      return "validation_json";
    case core::CustomFieldDefinition::Field::Indexed:
      return "indexed";
    case core::CustomFieldDefinition::Field::IsPhi:
      return "is_phi";
    case core::CustomFieldDefinition::Field::CreatedAt:
      return "created_at_micros";
    case core::CustomFieldDefinition::Field::ArchivedAt:
      return "archived_at_micros";
    }
    throw ConstraintViolation("unknown custom field definition field");
  }

  inline void validate_item_type(const core::ItemType& item_type) {
    if (item_type.name.empty()) {
      throw ConstraintViolation("item type name is required");
    }
  }

  // Pure shape validation for a CustomFieldDefinition. The item_type liveness
  // cross-reference is enforced separately by each backend's repository.
  inline void validate_cfd_shape(const core::CustomFieldDefinition& cfd) {
    if (cfd.key.empty()) {
      throw ConstraintViolation("custom field key is required");
    }
    if (cfd.label.empty()) {
      throw ConstraintViolation("custom field label is required");
    }
    if (cfd.is_phi && cfd.indexed) {
      throw ConstraintViolation("PHI fields may not be indexed (see L10.3)");
    }
  }

} // namespace fmgr::storage::detail

#endif // FMGR_STORAGE_DETAIL_ITEMTYPECOLUMNS_H
