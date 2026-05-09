// SPDX-License-Identifier: AGPL-3.0-or-later

// Geometry catalog for physical containers and their sample slot layouts.
// `ContainerType` describes a physical vessel (cryo-box, rack insert, etc.) including
// its outer dimensions and supplier SKU for procurement tracking.
// `BoxType` defines the grid of sample `Position`s inside a container, including which
// item types each slot accepts. Both are lab-scoped configuration records.
#ifndef FMGR_CORE_BOX_H
#define FMGR_CORE_BOX_H

#include "core/ids.h"
#include "core/timestamp.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace fmgr::core {
  namespace detail {

    // Duplicates `detail::optional_*` from identity.h to avoid a cross-header dependency.
    template <typename Value>
    [[nodiscard]] inline nlohmann::json box_optional_to_json(const std::optional<Value>& value) {
      if (!value.has_value()) {
        return nullptr;
      }
      return value.value();
    }

    template <typename Value>
    [[nodiscard]] inline std::optional<Value> box_optional_from_json(const nlohmann::json& json) {
      if (json.is_null()) {
        return std::nullopt;
      }
      return json.get<Value>();
    }

  } // namespace detail

  struct OuterDimensionsMm {
    double width{0.0};
    double height{0.0};
    double depth{0.0};

    friend bool operator==(const OuterDimensionsMm&, const OuterDimensionsMm&) = default;
  };

  struct ContainerType {
    using Id = ContainerTypeId;

    enum class Field : std::uint8_t {
      Id,
      LabId,
      Name,
      SizeClass,
      OuterDimensionsMm,
      Material,
      SupplierSku,
      CreatedAt,
      ArchivedAt,
    };

    ContainerTypeId id;
    LabId lab_id;
    std::string name;
    // Free-form string (e.g. `"9x9"`, `"10x10"`) used by the UI for grouping
    // and filtering. Not interpreted or validated by the backend.
    std::string size_class;
    std::optional<OuterDimensionsMm> outer_dimensions_mm;
    std::string material;
    std::string supplier_sku;
    Timestamp created_at;
    std::optional<Timestamp> archived_at;

    friend bool operator==(const ContainerType&, const ContainerType&) = default;
  };

  // A single sample slot within a `BoxType`. `label` is the human-readable
  // address shown in the UI (e.g. `"A1"`); `row`/`col` are 0-based grid
  // coordinates used for rendering.
  struct Position {
    std::string label;
    std::int32_t row{0};
    std::int32_t col{0};
    // Optional third dimension for layered or multi-deck storage formats.
    // Null for standard flat boxes.
    std::optional<std::int32_t> z;
    // Whitelist of `ItemType` keys permitted at this slot. Empty = unrestricted.
    std::vector<std::string> accepts;

    friend bool operator==(const Position&, const Position&) = default;
  };

  struct BoxType {
    using Id = BoxTypeId;

    enum class Field : std::uint8_t {
      Id,
      LabId,
      Name,
      Manufacturer,
      Sku,
      Positions,
      CreatedAt,
      ArchivedAt,
    };

    BoxTypeId id;
    LabId lab_id;
    std::string name;
    std::string manufacturer;
    std::string sku;
    std::vector<Position> positions;
    Timestamp created_at;
    std::optional<Timestamp> archived_at;

    friend bool operator==(const BoxType&, const BoxType&) = default;
  };

  inline void to_json(nlohmann::json& json, const OuterDimensionsMm& dimensions) {
    json = nlohmann::json{
        {"width", dimensions.width},
        {"height", dimensions.height},
        {"depth", dimensions.depth},
    };
  }

  inline void from_json(const nlohmann::json& json, OuterDimensionsMm& dimensions) {
    dimensions = OuterDimensionsMm{
        .width = json.at("width").get<double>(),
        .height = json.at("height").get<double>(),
        .depth = json.at("depth").get<double>(),
    };
  }

  inline void to_json(nlohmann::json& json, const ContainerType& container_type) {
    json = nlohmann::json{
        {"id", container_type.id},
        {"lab_id", container_type.lab_id},
        {"name", container_type.name},
        {"size_class", container_type.size_class},
        {"outer_dimensions_mm", detail::box_optional_to_json(container_type.outer_dimensions_mm)},
        {"material", container_type.material},
        {"supplier_sku", container_type.supplier_sku},
        {"created_at", container_type.created_at},
        {"archived_at", detail::box_optional_to_json(container_type.archived_at)},
    };
  }

  inline void from_json(const nlohmann::json& json, ContainerType& container_type) {
    container_type = ContainerType{
        .id = json.at("id").get<ContainerTypeId>(),
        .lab_id = json.at("lab_id").get<LabId>(),
        .name = json.at("name").get<std::string>(),
        .size_class = json.at("size_class").get<std::string>(),
        .outer_dimensions_mm =
            detail::box_optional_from_json<OuterDimensionsMm>(json.at("outer_dimensions_mm")),
        .material = json.at("material").get<std::string>(),
        .supplier_sku = json.at("supplier_sku").get<std::string>(),
        .created_at = json.at("created_at").get<Timestamp>(),
        .archived_at = detail::box_optional_from_json<Timestamp>(json.at("archived_at")),
    };
  }

  inline void to_json(nlohmann::json& json, const Position& position) {
    json = nlohmann::json{
        {"label", position.label},     {"row", position.row},
        {"col", position.col},         {"z", detail::box_optional_to_json(position.z)},
        {"accepts", position.accepts},
    };
  }

  inline void from_json(const nlohmann::json& json, Position& position) {
    position = Position{
        .label = json.at("label").get<std::string>(),
        .row = json.at("row").get<std::int32_t>(),
        .col = json.at("col").get<std::int32_t>(),
        .z = detail::box_optional_from_json<std::int32_t>(json.at("z")),
        .accepts = json.at("accepts").get<std::vector<std::string>>(),
    };
  }

  inline void to_json(nlohmann::json& json, const BoxType& box_type) {
    json = nlohmann::json{
        {"id", box_type.id},
        {"lab_id", box_type.lab_id},
        {"name", box_type.name},
        {"manufacturer", box_type.manufacturer},
        {"sku", box_type.sku},
        {"positions", box_type.positions},
        {"created_at", box_type.created_at},
        {"archived_at", detail::box_optional_to_json(box_type.archived_at)},
    };
  }

  inline void from_json(const nlohmann::json& json, BoxType& box_type) {
    box_type = BoxType{
        .id = json.at("id").get<BoxTypeId>(),
        .lab_id = json.at("lab_id").get<LabId>(),
        .name = json.at("name").get<std::string>(),
        .manufacturer = json.at("manufacturer").get<std::string>(),
        .sku = json.at("sku").get<std::string>(),
        .positions = json.at("positions").get<std::vector<Position>>(),
        .created_at = json.at("created_at").get<Timestamp>(),
        .archived_at = detail::box_optional_from_json<Timestamp>(json.at("archived_at")),
    };
  }

} // namespace fmgr::core

#endif // FMGR_CORE_BOX_H
