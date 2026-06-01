// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/freezer.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace fmgr::core {
  namespace {

    [[nodiscard]] Uuid uuid_from_low(std::uint64_t low_bits) {
      std::array<std::uint8_t, 16> bytes{};
      for (std::size_t index = 0; index < 8; ++index) {
        bytes.at(15 - index) = static_cast<std::uint8_t>((low_bits >> (index * 8U)) & 0xffU);
      }
      return Uuid(bytes);
    }

    template <typename StrongIdT> [[nodiscard]] StrongIdT id_from_low(std::uint64_t low_bits) {
      return StrongIdT(uuid_from_low(low_bits));
    }

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
