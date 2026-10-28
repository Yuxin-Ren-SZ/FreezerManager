// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/freezer.h"

#include "test_helpers.h"
#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace fmgr::core {
  namespace {
    using namespace fmgr::test;

    TEST(FreezerTypesTest, CapacityHintJsonRoundTripPreservesNullableFields) {
      const CapacityHint hint{.rows = 9, .cols = 9, .depth = std::nullopt};
      nlohmann::json json = hint;
      const auto restored = json.get<CapacityHint>();
      EXPECT_EQ(restored, hint);
      EXPECT_TRUE(json.at("depth").is_null());
    }

    TEST(FreezerTypesTest, FreezerJsonRoundTrip) {
      const Freezer freezer{
          .id = id_from_low<FreezerId>(1),
          .lab_id = id_from_low<LabId>(2),
          .name = "Minus 80",
          .location = "Room B-12",
          .model = "Thermo TSX",
          .temp_target_c = -80.0,
          .layout_root_id = id_from_low<StorageContainerId>(3),
          .created_at = Timestamp::from_unix_micros(123456),
      };
      nlohmann::json json = freezer;
      const auto restored = json.get<Freezer>();
      EXPECT_EQ(restored, freezer);
    }

    TEST(FreezerTypesTest, StorageContainerJsonRoundTripWithCapacityHint) {
      const StorageContainer container{
          .id = id_from_low<StorageContainerId>(10),
          .lab_id = id_from_low<LabId>(11),
          .parent_id = id_from_low<StorageContainerId>(12),
          .kind = ContainerKind::Rack,
          .name = "Rack A",
          .label = "A",
          .ordering_index = 2,
          .capacity_hint = CapacityHint{.rows = 4, .cols = 5},
          .created_at = Timestamp::from_unix_micros(7777),
      };
      nlohmann::json json = container;
      const auto restored = json.get<StorageContainer>();
      EXPECT_EQ(restored, container);
    }

    TEST(FreezerTypesTest, StorageContainerJsonRoundTripWithoutOptionalFields) {
      const StorageContainer container{
          .id = id_from_low<StorageContainerId>(20),
          .lab_id = id_from_low<LabId>(21),
          .kind = ContainerKind::Compartment,
          .name = "Top compartment",
          .label = "top",
          .created_at = Timestamp::from_unix_micros(1),
      };
      nlohmann::json json = container;
      EXPECT_TRUE(json.at("parent_id").is_null());
      EXPECT_TRUE(json.at("capacity_hint").is_null());
      const auto restored = json.get<StorageContainer>();
      EXPECT_EQ(restored, container);
    }

    TEST(FreezerTypesTest, FreezerJsonRoundTripCarriesTheFreezerModelId) {
      const Freezer freezer{
          .id = id_from_low<FreezerId>(30),
          .lab_id = id_from_low<LabId>(31),
          .name = "Minus 80 B",
          .layout_root_id = id_from_low<StorageContainerId>(32),
          .created_at = Timestamp::from_unix_micros(9),
          .freezer_model_id = "eppendorf-cryocube-f740hi",
      };
      nlohmann::json json = freezer;
      EXPECT_EQ(json.at("freezer_model_id"), "eppendorf-cryocube-f740hi");
      EXPECT_EQ(json.get<Freezer>(), freezer);
    }

    TEST(FreezerTypesTest, StorageContainerJsonRoundTripWithPlacementAndGrid) {
      const StorageContainer container{
          .id = id_from_low<StorageContainerId>(40),
          .lab_id = id_from_low<LabId>(41),
          .parent_id = id_from_low<StorageContainerId>(42),
          .kind = ContainerKind::Rack,
          .name = "Rack 4",
          .created_at = Timestamp::from_unix_micros(2),
          .placement = Placement{.level = 1, .column = 3, .depth = 0},
          .grid = GridSpec{.levels = 8, .columns = 1, .depths = 4},
      };
      nlohmann::json json = container;
      EXPECT_EQ(json.get<StorageContainer>(), container);
    }

    // Audit snapshots written before migration 15 have no placement, grid or
    // model keys; reading one back must yield the "unset" values, not throw.
    TEST(FreezerTypesTest, SnapshotsWithoutTheLayoutKeysReadBackAsUnset) {
      const Freezer freezer{
          .id = id_from_low<FreezerId>(50),
          .lab_id = id_from_low<LabId>(51),
          .name = "Legacy",
          .layout_root_id = id_from_low<StorageContainerId>(52),
          .created_at = Timestamp::from_unix_micros(3),
      };
      nlohmann::json freezer_json = freezer;
      freezer_json.erase("freezer_model_id");
      EXPECT_EQ(freezer_json.get<Freezer>().freezer_model_id, "");

      const StorageContainer container{
          .id = id_from_low<StorageContainerId>(53),
          .lab_id = id_from_low<LabId>(51),
          .name = "Legacy root",
          .created_at = Timestamp::from_unix_micros(3),
      };
      nlohmann::json container_json = container;
      container_json.erase("placement");
      container_json.erase("grid");
      const auto restored = container_json.get<StorageContainer>();
      EXPECT_FALSE(restored.placement.has_value());
      EXPECT_FALSE(restored.grid.has_value());
    }

    TEST(FreezerTypesTest, ContainerKindParsesAllVariants) {
      for (const auto kind : {ContainerKind::Compartment, ContainerKind::Shelf, ContainerKind::Rack,
                              ContainerKind::Drawer, ContainerKind::Custom}) {
        const auto text = std::string(to_string(kind));
        EXPECT_EQ(parse_container_kind(text), kind);
      }
    }

    TEST(FreezerTypesTest, ContainerKindRejectsEmptyString) {
      EXPECT_FALSE(to_string(core::ContainerKind::Compartment).empty());
      EXPECT_THROW(parse_container_kind(""), std::invalid_argument);
    }

    TEST(FreezerTypesTest, ContainerKindRejectsUnknownString) {
      EXPECT_THROW(parse_container_kind("not_a_valid_kind"), std::invalid_argument);
    }

  } // namespace
} // namespace fmgr::core
