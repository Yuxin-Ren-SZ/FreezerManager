// SPDX-License-Identifier: AGPL-3.0-or-later

// Physical appliance model and recursive interior layout.
// `Freezer` represents the physical unit (location, temperature target). Its interior
// is modeled as a tree of `StorageContainer` nodes rooted at `layout_root_id`; the
// root node is created atomically with the `Freezer` row and archived with it.
// This two-layer design keeps the appliance record stable while allowing arbitrary
// interior reorganization without touching the `Freezer` row itself.
#ifndef FMGR_CORE_FREEZER_H
#define FMGR_CORE_FREEZER_H

#include "core/enums.h"
#include "core/ids.h"
#include "core/timestamp.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace fmgr::core {
  namespace detail {

    // Duplicates `detail::optional_*` from identity.h to avoid pulling the entire
    // identity header into compilation units that only need the freezer layout types.
    template <typename Value>
    [[nodiscard]] inline nlohmann::json
    freezer_optional_to_json(const std::optional<Value>& value) {
      if (!value.has_value()) {
        return nullptr;
      }
      return value.value();
    }

    template <typename Value>
    [[nodiscard]] inline std::optional<Value>
    freezer_optional_from_json(const nlohmann::json& json) {
      if (json.is_null()) {
        return std::nullopt;
      }
      return json.get<Value>();
    }

  } // namespace detail

  // Advisory grid dimensions shown in the UI. Not an enforced constraint —
  // actual capacity is determined by the box types placed inside the container.
  // All three fields are optional; absent means unknown or not applicable.
  struct CapacityHint {
    std::optional<int> rows;
    std::optional<int> cols;
    std::optional<int> depth;

    friend bool operator==(const CapacityHint&, const CapacityHint&) = default;
  };

  inline void to_json(nlohmann::json& json, const CapacityHint& hint) {
    json = nlohmann::json{
        {"rows", detail::freezer_optional_to_json(hint.rows)},
        {"cols", detail::freezer_optional_to_json(hint.cols)},
        {"depth", detail::freezer_optional_to_json(hint.depth)},
    };
  }

  inline void from_json(const nlohmann::json& json, CapacityHint& hint) {
    hint = CapacityHint{
        .rows = detail::freezer_optional_from_json<int>(json.at("rows")),
        .cols = detail::freezer_optional_from_json<int>(json.at("cols")),
        .depth = detail::freezer_optional_from_json<int>(json.at("depth")),
    };
  }

  struct Freezer {
    using Id = FreezerId;

    enum class Field : std::uint8_t {
      Id,
      LabId,
      Name,
      Location,
      Model,
      TempTargetC,
      LayoutRootId,
      CreatedAt,
      ArchivedAt,
    };

    FreezerId id;
    LabId lab_id;
    std::string name;
    std::string location;
    std::string model;
    std::optional<double> temp_target_c;
    // References the root `StorageContainer` (`parent_id = null`) for this appliance.
    // Created in the same transaction as the `Freezer` row; archived together.
    StorageContainerId layout_root_id;
    Timestamp created_at;
    std::optional<Timestamp> archived_at;

    friend bool operator==(const Freezer&, const Freezer&) = default;
  };

  struct StorageContainer {
    using Id = StorageContainerId;

    enum class Field : std::uint8_t {
      Id,
      LabId,
      ParentId,
      Kind,
      Name,
      Label,
      OrderingIndex,
      CapacityHint,
      CreatedAt,
      ArchivedAt,
    };

    StorageContainerId id;
    LabId lab_id;
    // Null only on the root node (the one referenced by `Freezer.layout_root_id`).
    // All other nodes reference their direct parent within the same lab.
    std::optional<StorageContainerId> parent_id;
    ContainerKind kind{ContainerKind::Custom};
    std::string name;
    std::string label;
    // Sibling position within a parent; lower index renders first in the UI.
    // Gaps are valid — indices need not be contiguous after reordering operations.
    std::int32_t ordering_index{0};
    std::optional<core::CapacityHint> capacity_hint;
    Timestamp created_at;
    std::optional<Timestamp> archived_at;

    friend bool operator==(const StorageContainer&, const StorageContainer&) = default;
  };

  inline void to_json(nlohmann::json& json, const Freezer& freezer) {
    json = nlohmann::json{
        {"id", freezer.id},
        {"lab_id", freezer.lab_id},
        {"name", freezer.name},
        {"location", freezer.location},
        {"model", freezer.model},
        {"temp_target_c", detail::freezer_optional_to_json(freezer.temp_target_c)},
        {"layout_root_id", freezer.layout_root_id},
        {"created_at", freezer.created_at},
        {"archived_at", detail::freezer_optional_to_json(freezer.archived_at)},
    };
  }

  inline void from_json(const nlohmann::json& json, Freezer& freezer) {
    freezer = Freezer{
        .id = json.at("id").get<FreezerId>(),
        .lab_id = json.at("lab_id").get<LabId>(),
        .name = json.at("name").get<std::string>(),
        .location = json.at("location").get<std::string>(),
        .model = json.at("model").get<std::string>(),
        .temp_target_c = detail::freezer_optional_from_json<double>(json.at("temp_target_c")),
        .layout_root_id = json.at("layout_root_id").get<StorageContainerId>(),
        .created_at = json.at("created_at").get<Timestamp>(),
        .archived_at = detail::freezer_optional_from_json<Timestamp>(json.at("archived_at")),
    };
  }

  inline void to_json(nlohmann::json& json, const StorageContainer& container) {
    json = nlohmann::json{
        {"id", container.id},
        {"lab_id", container.lab_id},
        {"parent_id", detail::freezer_optional_to_json(container.parent_id)},
        {"kind", container.kind},
        {"name", container.name},
        {"label", container.label},
        {"ordering_index", container.ordering_index},
        {"capacity_hint", detail::freezer_optional_to_json(container.capacity_hint)},
        {"created_at", container.created_at},
        {"archived_at", detail::freezer_optional_to_json(container.archived_at)},
    };
  }

  inline void from_json(const nlohmann::json& json, StorageContainer& container) {
    container = StorageContainer{
        .id = json.at("id").get<StorageContainerId>(),
        .lab_id = json.at("lab_id").get<LabId>(),
        .parent_id = detail::freezer_optional_from_json<StorageContainerId>(json.at("parent_id")),
        .kind = json.at("kind").get<ContainerKind>(),
        .name = json.at("name").get<std::string>(),
        .label = json.at("label").get<std::string>(),
        .ordering_index = json.at("ordering_index").get<std::int32_t>(),
        .capacity_hint = detail::freezer_optional_from_json<CapacityHint>(json.at("capacity_hint")),
        .created_at = json.at("created_at").get<Timestamp>(),
        .archived_at = detail::freezer_optional_from_json<Timestamp>(json.at("archived_at")),
    };
  }

} // namespace fmgr::core

#endif // FMGR_CORE_FREEZER_H
