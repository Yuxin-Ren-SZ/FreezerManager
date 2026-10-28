// SPDX-License-Identifier: AGPL-3.0-or-later

// Built-in freezer model catalogue (ADR 0003 §1).
// A freezer model is the shell and fixed interior a 3D layout draws: outside and
// inside dimensions, door hinge, inner doors and the compartments that hold
// racks. The catalogue is constant data versioned with the renderers that draw
// each entry (`render_key`), not a database table: built-in rows would need RLS
// exceptions on lab-scoped tables, and lab-custom models have no consumer yet.
// `Freezer.freezer_model_id` names an entry by its stable `id`.
//
// Adding a model: append an entry here (dimensions in millimetres, compartments
// bottom -> top), and `FreezerModelsTest.EveryBuiltinModelPassesValidation`
// checks it.
#ifndef FMGR_CORE_FREEZER_MODELS_H
#define FMGR_CORE_FREEZER_MODELS_H

#include "core/placement.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fmgr::core {

  enum class FreezerForm : std::uint8_t { Upright, Chest };
  enum class HingeSide : std::uint8_t { Left, Right };

  struct Dimensions {
    std::int32_t width_mm{0};
    std::int32_t depth_mm{0};
    std::int32_t height_mm{0};

    friend bool operator==(const Dimensions&, const Dimensions&) = default;
  };

  // One compartment, between two shelves (or the floor and a shelf). Racks stand
  // side by side in `rack_slots` positions, left -> right.
  struct CompartmentSpec {
    std::int32_t level{0};    // 0 = bottom
    std::int32_t floor_mm{0}; // above the interior floor
    std::int32_t height_mm{0};
    std::int32_t rack_slots{0};
    Dimensions rack_slot_mm;

    friend bool operator==(const CompartmentSpec&, const CompartmentSpec&) = default;
  };

  // One insulated inner door and the compartment levels it closes off. A door
  // may span several levels (the TSX40086A has 2 doors over 4 levels).
  struct InnerDoorSpec {
    std::int32_t first_level{0};
    std::int32_t last_level{0};

    friend bool operator==(const InnerDoorSpec&, const InnerDoorSpec&) = default;
  };

  struct FreezerModel {
    std::string id; // stable key stored in Freezer.freezer_model_id
    std::string manufacturer;
    std::string model_code;
    std::string display_name;
    FreezerForm form{FreezerForm::Upright};
    Dimensions exterior_mm;
    Dimensions interior_mm;
    HingeSide hinge{HingeSide::Right};         // as seen from the front
    std::vector<InnerDoorSpec> inner_doors;    // bottom -> top; empty = none
    std::vector<CompartmentSpec> compartments; // bottom -> top
    std::string render_key;                    // hand-built renderer; "" = generic cabinet

    friend bool operator==(const FreezerModel&, const FreezerModel&) = default;
  };

  [[nodiscard]] inline const std::vector<FreezerModel>& builtin_freezer_models() {
    static const std::vector<FreezerModel> models{
        // Eppendorf CryoCube F740hi: 740 L, 3 compartments behind 3 inner doors,
        // 6 side-access racks each (8 tiers x 4 deep, 576 two-inch boxes).
        FreezerModel{
            .id = "eppendorf-cryocube-f740hi",
            .manufacturer = "Eppendorf",
            .model_code = "F740hi",
            .display_name = "CryoCube F740hi",
            .form = FreezerForm::Upright,
            .exterior_mm = {.width_mm = 1099, .depth_mm = 980, .height_mm = 1973},
            .interior_mm = {.width_mm = 865, .depth_mm = 622, .height_mm = 1390},
            .hinge = HingeSide::Right,
            .inner_doors = {{.first_level = 0, .last_level = 0},
                            {.first_level = 1, .last_level = 1},
                            {.first_level = 2, .last_level = 2}},
            .compartments =
                {
                    {.level = 0,
                     .floor_mm = 0,
                     .height_mm = 457,
                     .rack_slots = 6,
                     .rack_slot_mm = {.width_mm = 139, .depth_mm = 560, .height_mm = 430}},
                    {.level = 1,
                     .floor_mm = 466,
                     .height_mm = 457,
                     .rack_slots = 6,
                     .rack_slot_mm = {.width_mm = 139, .depth_mm = 560, .height_mm = 430}},
                    {.level = 2,
                     .floor_mm = 932,
                     .height_mm = 457,
                     .rack_slots = 6,
                     .rack_slot_mm = {.width_mm = 139, .depth_mm = 560, .height_mm = 430}},
                },
            .render_key = "cryocube-f740",
        },
        // Thermo Scientific TSX Universal TSX40086A: 4 levels behind 2 inner doors
        // (levels 0-1 and 2-3), 4 drawer racks each (5 drawers x 5 boxes).
        FreezerModel{
            .id = "thermo-tsx40086a",
            .manufacturer = "Thermo Scientific",
            .model_code = "TSX40086A",
            .display_name = "TSX Universal TSX40086A",
            .form = FreezerForm::Upright,
            .exterior_mm = {.width_mm = 719, .depth_mm = 955, .height_mm = 1981},
            .interior_mm = {.width_mm = 588, .depth_mm = 719, .height_mm = 1301},
            .hinge = HingeSide::Right,
            .inner_doors = {{.first_level = 0, .last_level = 1},
                            {.first_level = 2, .last_level = 3}},
            .compartments =
                {
                    {.level = 0,
                     .floor_mm = 0,
                     .height_mm = 320,
                     .rack_slots = 4,
                     .rack_slot_mm = {.width_mm = 136, .depth_mm = 665, .height_mm = 295}},
                    {.level = 1,
                     .floor_mm = 325,
                     .height_mm = 320,
                     .rack_slots = 4,
                     .rack_slot_mm = {.width_mm = 136, .depth_mm = 665, .height_mm = 295}},
                    {.level = 2,
                     .floor_mm = 650,
                     .height_mm = 320,
                     .rack_slots = 4,
                     .rack_slot_mm = {.width_mm = 136, .depth_mm = 665, .height_mm = 295}},
                    {.level = 3,
                     .floor_mm = 975,
                     .height_mm = 320,
                     .rack_slots = 4,
                     .rack_slot_mm = {.width_mm = 136, .depth_mm = 665, .height_mm = 295}},
                },
            .render_key = "tsx-40086",
        },
    };
    return models;
  }

  // The catalogue entry with this id, or nullptr. The pointer stays valid for the
  // life of the process.
  [[nodiscard]] inline const FreezerModel* find_freezer_model(std::string_view model_id) {
    for (const auto& model : builtin_freezer_models()) {
      if (model.id == model_id) {
        return &model;
      }
    }
    return nullptr;
  }

  // The cells a freezer's layout root offers its direct children: one level per
  // compartment, that compartment's rack slots wide, one rack deep.
  [[nodiscard]] inline PlacementBounds cabinet_bounds(const FreezerModel& model) {
    PlacementBounds bounds{.columns_per_level = {}, .depths = 1};
    bounds.columns_per_level.reserve(model.compartments.size());
    for (const auto& compartment : model.compartments) {
      bounds.columns_per_level.push_back(compartment.rack_slots);
    }
    return bounds;
  }

  namespace detail {
    [[nodiscard]] inline bool positive(const Dimensions& dimensions) {
      return dimensions.width_mm > 0 && dimensions.depth_mm > 0 && dimensions.height_mm > 0;
    }
  } // namespace detail

  // Every way `model` is not a drawable, self-consistent cabinet; empty when it is.
  [[nodiscard]] inline std::vector<std::string> freezer_model_errors(const FreezerModel& model) {
    std::vector<std::string> errors;
    if (model.id.empty() || model.display_name.empty()) {
      errors.emplace_back("model id and display name are required");
    }
    if (!detail::positive(model.exterior_mm) || !detail::positive(model.interior_mm)) {
      errors.emplace_back("dimensions must be positive");
    }
    if (model.interior_mm.width_mm > model.exterior_mm.width_mm ||
        model.interior_mm.depth_mm > model.exterior_mm.depth_mm ||
        model.interior_mm.height_mm > model.exterior_mm.height_mm) {
      errors.emplace_back("interior must fit inside the exterior");
    }
    if (model.compartments.empty() ||
        model.compartments.size() > static_cast<std::size_t>(k_max_grid_extent)) {
      errors.emplace_back("a model needs between 1 and 64 compartments");
    }

    std::int32_t previous_top = 0;
    for (std::size_t index = 0; index < model.compartments.size(); ++index) {
      const auto& compartment = model.compartments[index];
      const auto where = "compartment " + std::to_string(index);
      if (compartment.level != static_cast<std::int32_t>(index)) {
        errors.push_back(where + ": levels must run 0..n-1 bottom -> top");
      }
      if (compartment.floor_mm < previous_top || compartment.height_mm <= 0) {
        errors.push_back(where + ": overlaps the compartment below or has no height");
      }
      previous_top = compartment.floor_mm + compartment.height_mm;
      if (previous_top > model.interior_mm.height_mm) {
        errors.push_back(where + ": rises above the interior");
      }
      if (compartment.rack_slots < 1 || compartment.rack_slots > k_max_grid_extent) {
        errors.push_back(where + ": rack slots must be between 1 and 64");
      }
      if (!detail::positive(compartment.rack_slot_mm)) {
        errors.push_back(where + ": rack slot dimensions must be positive");
      }
      if (compartment.rack_slots * compartment.rack_slot_mm.width_mm > model.interior_mm.width_mm ||
          compartment.rack_slot_mm.depth_mm > model.interior_mm.depth_mm ||
          compartment.rack_slot_mm.height_mm > compartment.height_mm) {
        errors.push_back(where + ": racks do not fit in the compartment");
      }
    }

    const auto levels = static_cast<std::int32_t>(model.compartments.size());
    std::int32_t next_free_level = 0;
    for (const auto& door : model.inner_doors) {
      if (door.first_level > door.last_level || door.first_level < next_free_level ||
          door.last_level >= levels) {
        errors.emplace_back("inner doors must cover existing levels, bottom -> top, "
                            "without overlapping");
      }
      next_free_level = door.last_level + 1;
    }
    return errors;
  }

} // namespace fmgr::core

#endif // FMGR_CORE_FREEZER_MODELS_H
