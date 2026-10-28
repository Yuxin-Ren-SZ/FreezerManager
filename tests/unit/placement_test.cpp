// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/placement.h"

#include <gtest/gtest.h>

#include <vector>

namespace fmgr::core {
  namespace {

    TEST(PlacementTest, PlacementAndGridJsonRoundTrip) {
      const Placement placement{.level = 2, .column = 5, .depth = 1};
      nlohmann::json placement_json = placement;
      EXPECT_EQ(placement_json.get<Placement>(), placement);

      const GridSpec grid{.levels = 8, .columns = 1, .depths = 4};
      nlohmann::json grid_json = grid;
      EXPECT_EQ(grid_json.get<GridSpec>(), grid);
    }

    TEST(PlacementTest, GridDimensionsMustBeBetweenOneAndTheMaximum) {
      EXPECT_FALSE(grid_error(GridSpec{.levels = 8, .columns = 1, .depths = 4}).has_value());
      EXPECT_FALSE(grid_error(GridSpec{.levels = k_max_grid_extent,
                                       .columns = k_max_grid_extent,
                                       .depths = k_max_grid_extent})
                       .has_value());
      EXPECT_TRUE(grid_error(GridSpec{.levels = 0, .columns = 1, .depths = 1}).has_value());
      EXPECT_TRUE(grid_error(GridSpec{.levels = 1, .columns = -1, .depths = 1}).has_value());
      EXPECT_TRUE(
          grid_error(GridSpec{.levels = 1, .columns = 1, .depths = k_max_grid_extent + 1})
              .has_value());
    }

    TEST(PlacementTest, RectangularGridBoundsContainOnlyCellsInsideTheGrid) {
      const auto bounds = bounds_of(GridSpec{.levels = 8, .columns = 1, .depths = 4});
      EXPECT_TRUE(bounds.contains(Placement{.level = 0, .column = 0, .depth = 0}));
      EXPECT_TRUE(bounds.contains(Placement{.level = 7, .column = 0, .depth = 3}));
      EXPECT_FALSE(bounds.contains(Placement{.level = 8, .column = 0, .depth = 0}));
      EXPECT_FALSE(bounds.contains(Placement{.level = 0, .column = 1, .depth = 0}));
      EXPECT_FALSE(bounds.contains(Placement{.level = 0, .column = 0, .depth = 4}));
      EXPECT_FALSE(bounds.contains(Placement{.level = -1, .column = 0, .depth = 0}));
    }

    TEST(PlacementTest, BoundsWithPerLevelColumnsRespectEachLevelsWidth) {
      // A cabinet whose bottom compartment is narrower than the top one.
      const PlacementBounds bounds{.columns_per_level = {2, 6}, .depths = 1};
      EXPECT_TRUE(bounds.contains(Placement{.level = 0, .column = 1, .depth = 0}));
      EXPECT_FALSE(bounds.contains(Placement{.level = 0, .column = 2, .depth = 0}));
      EXPECT_TRUE(bounds.contains(Placement{.level = 1, .column = 5, .depth = 0}));
      EXPECT_FALSE(bounds.contains(Placement{.level = 1, .column = 0, .depth = 1}));
    }

    TEST(PlacementTest, PlacementWithoutParentGridIsRefused) {
      const auto error = placement_error(std::nullopt, Placement{});
      ASSERT_TRUE(error.has_value());
      EXPECT_NE(error->find("grid"), std::string::npos);
    }

    TEST(PlacementTest, PlacementOutsideParentGridIsRefusedAndInsideIsAccepted) {
      const auto bounds = bounds_of(GridSpec{.levels = 5, .columns = 1, .depths = 5});
      EXPECT_FALSE(placement_error(bounds, Placement{.level = 4, .column = 0, .depth = 4}));
      EXPECT_TRUE(placement_error(bounds, Placement{.level = 5, .column = 0, .depth = 0}));
    }

    TEST(PlacementTest, StrandedChildrenAreThoseOutsideTheNewBounds) {
      const std::vector<Placement> children{
          Placement{.level = 0, .column = 0, .depth = 0},
          Placement{.level = 3, .column = 0, .depth = 2},
          Placement{.level = 1, .column = 0, .depth = 3},
      };
      const auto shrunk = bounds_of(GridSpec{.levels = 2, .columns = 1, .depths = 3});
      const auto stranded = stranded_placements(shrunk, children);
      ASSERT_EQ(stranded.size(), 2U);
      EXPECT_EQ(stranded[0], children[1]);
      EXPECT_EQ(stranded[1], children[2]);

      const auto same = bounds_of(GridSpec{.levels = 4, .columns = 1, .depths = 4});
      EXPECT_TRUE(stranded_placements(same, children).empty());
    }

  } // namespace
} // namespace fmgr::core
