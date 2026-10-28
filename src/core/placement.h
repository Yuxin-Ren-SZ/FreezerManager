// SPDX-License-Identifier: AGPL-3.0-or-later

// Typed physical placement for the 3D layout (ADR 0003 §2).
// A `Placement` is where a container or box sits inside its parent; a `GridSpec`
// is the grid a container offers its children. Both replace the free-form
// `CapacityHint`. A parent that is a freezer's layout root has no stored grid:
// its bounds come from the freezer model (see `cabinet_bounds` in
// freezer_models.h), where each compartment has its own number of rack slots.
// That is why bounds are expressed as `PlacementBounds` (columns per level)
// rather than as a plain `GridSpec`.
#ifndef FMGR_CORE_PLACEMENT_H
#define FMGR_CORE_PLACEMENT_H

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace fmgr::core {

  // Upper bound on every grid dimension. Large enough for any real rack or
  // drawer; small enough that a client can render every cell.
  inline constexpr std::int32_t k_max_grid_extent = 64;

  struct Placement {
    std::int32_t level{0};  // vertical, 0 = bottom
    std::int32_t column{0}; // left -> right as seen from the front
    std::int32_t depth{0};  // front -> back, 0 = front

    friend bool operator==(const Placement&, const Placement&) = default;
  };

  struct GridSpec {
    std::int32_t levels{1};
    std::int32_t columns{1};
    std::int32_t depths{1};

    friend bool operator==(const GridSpec&, const GridSpec&) = default;
  };

  inline void to_json(nlohmann::json& json, const Placement& placement) {
    json = nlohmann::json{
        {"level", placement.level},
        {"column", placement.column},
        {"depth", placement.depth},
    };
  }

  inline void from_json(const nlohmann::json& json, Placement& placement) {
    placement = Placement{
        .level = json.at("level").get<std::int32_t>(),
        .column = json.at("column").get<std::int32_t>(),
        .depth = json.at("depth").get<std::int32_t>(),
    };
  }

  inline void to_json(nlohmann::json& json, const GridSpec& grid) {
    json = nlohmann::json{
        {"levels", grid.levels},
        {"columns", grid.columns},
        {"depths", grid.depths},
    };
  }

  inline void from_json(const nlohmann::json& json, GridSpec& grid) {
    grid = GridSpec{
        .levels = json.at("levels").get<std::int32_t>(),
        .columns = json.at("columns").get<std::int32_t>(),
        .depths = json.at("depths").get<std::int32_t>(),
    };
  }

  // The cells a parent offers its children. `columns_per_level[l]` is the width
  // of level `l`; every level shares the same depth.
  struct PlacementBounds {
    std::vector<std::int32_t> columns_per_level;
    std::int32_t depths{1};

    [[nodiscard]] bool contains(const Placement& placement) const {
      if (placement.level < 0 || placement.column < 0 || placement.depth < 0) {
        return false;
      }
      const auto level = static_cast<std::size_t>(placement.level);
      if (level >= columns_per_level.size()) {
        return false;
      }
      return placement.column < columns_per_level[level] && placement.depth < depths;
    }

    friend bool operator==(const PlacementBounds&, const PlacementBounds&) = default;
  };

  [[nodiscard]] inline PlacementBounds bounds_of(const GridSpec& grid) {
    return PlacementBounds{
        .columns_per_level =
            std::vector<std::int32_t>(static_cast<std::size_t>(std::max(grid.levels, 0)),
                                      grid.columns),
        .depths = grid.depths,
    };
  }

  // Why `grid` is not a usable grid, or nullopt when it is.
  [[nodiscard]] inline std::optional<std::string> grid_error(const GridSpec& grid) {
    const auto in_range = [](std::int32_t extent) {
      return extent >= 1 && extent <= k_max_grid_extent;
    };
    if (!in_range(grid.levels) || !in_range(grid.columns) || !in_range(grid.depths)) {
      return "grid dimensions must be between 1 and " + std::to_string(k_max_grid_extent);
    }
    return std::nullopt;
  }

  // Why `placement` cannot go under a parent with `parent_bounds`, or nullopt
  // when it can. A parent without a grid accepts no placed children.
  [[nodiscard]] inline std::optional<std::string>
  placement_error(const std::optional<PlacementBounds>& parent_bounds,
                  const Placement& placement) {
    if (!parent_bounds.has_value()) {
      return "placement requires a parent with a grid";
    }
    if (!parent_bounds->contains(placement)) {
      return "placement is outside the parent grid";
    }
    return std::nullopt;
  }

  // The placements in `children` that `bounds` would no longer contain, in
  // input order. A grid change or a model change that strands a child is refused.
  [[nodiscard]] inline std::vector<Placement>
  stranded_placements(const PlacementBounds& bounds, const std::vector<Placement>& children) {
    std::vector<Placement> stranded;
    for (const auto& child : children) {
      if (!bounds.contains(child)) {
        stranded.push_back(child);
      }
    }
    return stranded;
  }

} // namespace fmgr::core

#endif // FMGR_CORE_PLACEMENT_H
