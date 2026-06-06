// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_STORAGE_DETAIL_BOXGEOMETRYCOLUMNS_H
#define FMGR_STORAGE_DETAIL_BOXGEOMETRYCOLUMNS_H

#include "core/box.h"
#include "storage/IStorageBackend.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <set>
#include <string>
#include <string_view>

// Backend-neutral column-name mapping, pure validation, and dimension (de)serialization
// for the box-geometry entities (ContainerType, BoxType, Box). Column names are identical
// across both schemas. Cross-entity validation that needs a query (size_class liveness,
// box parent references) stays in each backend's repository.
namespace fmgr::storage::detail {

  [[nodiscard]] inline std::string container_type_column_name(core::ContainerType::Field field) {
    switch (field) {
    case core::ContainerType::Field::Id:
      return "id";
    case core::ContainerType::Field::LabId:
      return "lab_id";
    case core::ContainerType::Field::Name:
      return "name";
    case core::ContainerType::Field::SizeClass:
      return "size_class";
    case core::ContainerType::Field::OuterDimensionsMm:
      return "outer_dimensions_json";
    case core::ContainerType::Field::Material:
      return "material";
    case core::ContainerType::Field::SupplierSku:
      return "supplier_sku";
    case core::ContainerType::Field::CreatedAt:
      return "created_at_micros";
    case core::ContainerType::Field::ArchivedAt:
      return "archived_at_micros";
    }
    throw ConstraintViolation("unknown container type field");
  }

  [[nodiscard]] inline std::string box_type_column_name(core::BoxType::Field field) {
    switch (field) {
    case core::BoxType::Field::Id:
      return "id";
    case core::BoxType::Field::LabId:
      return "lab_id";
    case core::BoxType::Field::Name:
      return "name";
    case core::BoxType::Field::Manufacturer:
      return "manufacturer";
    case core::BoxType::Field::Sku:
      return "sku";
    case core::BoxType::Field::Positions:
      throw UnsupportedOperation("box type position queries are not supported");
    case core::BoxType::Field::CreatedAt:
      return "created_at_micros";
    case core::BoxType::Field::ArchivedAt:
      return "archived_at_micros";
    }
    throw ConstraintViolation("unknown box type field");
  }

  [[nodiscard]] inline std::string box_column_name(core::Box::Field field) {
    switch (field) {
    case core::Box::Field::Id:
      return "id";
    case core::Box::Field::LabId:
      return "lab_id";
    case core::Box::Field::BoxTypeId:
      return "box_type_id";
    case core::Box::Field::StorageContainerId:
      return "storage_container_id";
    case core::Box::Field::Label:
      return "label";
    case core::Box::Field::Serial:
      return "serial";
    case core::Box::Field::Barcode:
      return "barcode";
    case core::Box::Field::CreatedAt:
      return "created_at_micros";
    case core::Box::Field::ArchivedAt:
      return "archived_at_micros";
    }
    throw ConstraintViolation("unknown box field");
  }

  [[nodiscard]] inline std::string
  dimensions_dump(const std::optional<core::OuterDimensionsMm>& dimensions) {
    if (!dimensions.has_value()) {
      return "null";
    }
    const nlohmann::json json = dimensions.value();
    return json.dump();
  }

  [[nodiscard]] inline std::optional<core::OuterDimensionsMm>
  parse_dimensions(std::string_view text) {
    const auto json = nlohmann::json::parse(text);
    if (json.is_null()) {
      return std::nullopt;
    }
    return json.get<core::OuterDimensionsMm>();
  }

  inline void validate_container_type(const core::ContainerType& container_type) {
    if (container_type.name.empty()) {
      throw ConstraintViolation("container type name is required");
    }
    if (container_type.size_class.empty()) {
      throw ConstraintViolation("container type size_class is required");
    }
    if (container_type.outer_dimensions_mm.has_value()) {
      const auto& dimensions = container_type.outer_dimensions_mm.value();
      if (dimensions.width <= 0.0 || dimensions.height <= 0.0 || dimensions.depth <= 0.0) {
        throw ConstraintViolation("container type dimensions must be positive");
      }
    }
  }

  inline void validate_box_type_shape(const core::BoxType& box_type) {
    if (box_type.name.empty()) {
      throw ConstraintViolation("box type name is required");
    }
    std::set<std::string> labels;
    for (const auto& position : box_type.positions) {
      if (position.label.empty()) {
        throw ConstraintViolation("box type position label is required");
      }
      if (!labels.insert(position.label).second) {
        throw ConstraintViolation("box type position labels must be unique");
      }
      if (position.row < 0 || position.col < 0 || (position.z.has_value() && *position.z < 0)) {
        throw ConstraintViolation("box type position coordinates must be non-negative");
      }
      if (position.accepts.empty()) {
        throw ConstraintViolation("box type position accepts must be non-empty");
      }
      std::set<std::string> accepts;
      for (const auto& size_class : position.accepts) {
        if (size_class.empty()) {
          throw ConstraintViolation("box type position accepts must be non-empty");
        }
        if (!accepts.insert(size_class).second) {
          throw ConstraintViolation("box type position accepts must be unique");
        }
      }
    }
  }

} // namespace fmgr::storage::detail

#endif // FMGR_STORAGE_DETAIL_BOXGEOMETRYCOLUMNS_H
