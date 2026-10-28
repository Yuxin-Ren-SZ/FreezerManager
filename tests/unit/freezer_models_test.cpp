// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/freezer_models.h"

#include <gtest/gtest.h>

#include <set>
#include <string>

namespace fmgr::core {
  namespace {

    TEST(FreezerModelsTest, EveryBuiltinModelPassesValidation) {
      ASSERT_FALSE(builtin_freezer_models().empty());
      for (const auto& model : builtin_freezer_models()) {
        const auto errors = freezer_model_errors(model);
        EXPECT_TRUE(errors.empty()) << model.id << ": " << (errors.empty() ? "" : errors.front());
      }
    }

    TEST(FreezerModelsTest, BuiltinModelIdsAreUnique) {
      std::set<std::string> ids;
      for (const auto& model : builtin_freezer_models()) {
        EXPECT_TRUE(ids.insert(model.id).second) << model.id;
      }
    }

    TEST(FreezerModelsTest, CryoCubeF740HasThreeCompartmentsOfSixRackSlots) {
      const auto* model = find_freezer_model("eppendorf-cryocube-f740hi");
      ASSERT_NE(model, nullptr);
      EXPECT_EQ(model->render_key, "cryocube-f740");
      EXPECT_EQ(model->hinge, HingeSide::Right);
      ASSERT_EQ(model->compartments.size(), 3U);
      for (const auto& compartment : model->compartments) {
        EXPECT_EQ(compartment.rack_slots, 6);
      }
      EXPECT_EQ(model->inner_doors.size(), 3U);
    }

    TEST(FreezerModelsTest, Tsx40086HasTwoInnerDoorsEachSpanningTwoLevels) {
      const auto* model = find_freezer_model("thermo-tsx40086a");
      ASSERT_NE(model, nullptr);
      EXPECT_EQ(model->render_key, "tsx-40086");
      ASSERT_EQ(model->compartments.size(), 4U);
      ASSERT_EQ(model->inner_doors.size(), 2U);
      EXPECT_EQ(model->inner_doors[0], (InnerDoorSpec{.first_level = 0, .last_level = 1}));
      EXPECT_EQ(model->inner_doors[1], (InnerDoorSpec{.first_level = 2, .last_level = 3}));
    }

    TEST(FreezerModelsTest, UnknownModelIdIsNotFound) {
      EXPECT_EQ(find_freezer_model(""), nullptr);
      EXPECT_EQ(find_freezer_model("no-such-model"), nullptr);
    }

    TEST(FreezerModelsTest, CabinetBoundsFollowEachCompartmentsRackSlots) {
      const auto* model = find_freezer_model("thermo-tsx40086a");
      ASSERT_NE(model, nullptr);
      const auto bounds = cabinet_bounds(*model);
      EXPECT_TRUE(bounds.contains(Placement{.level = 3, .column = 3, .depth = 0}));
      EXPECT_FALSE(bounds.contains(Placement{.level = 4, .column = 0, .depth = 0}));
      EXPECT_FALSE(bounds.contains(Placement{.level = 0, .column = 4, .depth = 0}));
      // Racks stand side by side in a compartment; there is no second row behind them.
      EXPECT_FALSE(bounds.contains(Placement{.level = 0, .column = 0, .depth = 1}));
    }

    // Planted bad specs: each must be caught, so the catalogue test above means something.
    class FreezerModelValidationTest : public ::testing::Test {
    protected:
      [[nodiscard]] static FreezerModel valid_model() {
        const auto* model = find_freezer_model("thermo-tsx40086a");
        EXPECT_NE(model, nullptr);
        return *model;
      }
    };

    TEST_F(FreezerModelValidationTest, MissingIdIsRefused) {
      auto model = valid_model();
      model.id.clear();
      EXPECT_FALSE(freezer_model_errors(model).empty());
    }

    TEST_F(FreezerModelValidationTest, NonPositiveDimensionIsRefused) {
      auto model = valid_model();
      model.interior_mm.depth_mm = 0;
      EXPECT_FALSE(freezer_model_errors(model).empty());
    }

    TEST_F(FreezerModelValidationTest, InteriorLargerThanExteriorIsRefused) {
      auto model = valid_model();
      model.interior_mm.width_mm = model.exterior_mm.width_mm + 1;
      EXPECT_FALSE(freezer_model_errors(model).empty());
    }

    TEST_F(FreezerModelValidationTest, CompartmentLevelGapIsRefused) {
      auto model = valid_model();
      model.compartments[2].level = 5;
      EXPECT_FALSE(freezer_model_errors(model).empty());
    }

    TEST_F(FreezerModelValidationTest, OverlappingCompartmentsAreRefused) {
      auto model = valid_model();
      model.compartments[1].floor_mm = model.compartments[0].floor_mm + 10;
      EXPECT_FALSE(freezer_model_errors(model).empty());
    }

    TEST_F(FreezerModelValidationTest, CompartmentAboveTheInteriorIsRefused) {
      auto model = valid_model();
      model.compartments.back().height_mm = model.interior_mm.height_mm;
      EXPECT_FALSE(freezer_model_errors(model).empty());
    }

    TEST_F(FreezerModelValidationTest, RackSlotsWiderThanTheInteriorAreRefused) {
      auto model = valid_model();
      model.compartments[0].rack_slots = 40;
      EXPECT_FALSE(freezer_model_errors(model).empty());
    }

    TEST_F(FreezerModelValidationTest, RackTallerThanItsCompartmentIsRefused) {
      auto model = valid_model();
      model.compartments[0].rack_slot_mm.height_mm = model.compartments[0].height_mm + 1;
      EXPECT_FALSE(freezer_model_errors(model).empty());
    }

    TEST_F(FreezerModelValidationTest, InnerDoorOutOfRangeIsRefused) {
      auto model = valid_model();
      model.inner_doors[1].last_level = 4;
      EXPECT_FALSE(freezer_model_errors(model).empty());
    }

    TEST_F(FreezerModelValidationTest, OverlappingInnerDoorsAreRefused) {
      auto model = valid_model();
      model.inner_doors[1].first_level = 1;
      EXPECT_FALSE(freezer_model_errors(model).empty());
    }

    TEST_F(FreezerModelValidationTest, InvertedInnerDoorRangeIsRefused) {
      auto model = valid_model();
      model.inner_doors[0] = InnerDoorSpec{.first_level = 1, .last_level = 0};
      EXPECT_FALSE(freezer_model_errors(model).empty());
    }

  } // namespace
} // namespace fmgr::core
