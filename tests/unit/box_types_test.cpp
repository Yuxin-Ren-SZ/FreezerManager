// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/box.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

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

    TEST(BoxTypesTest, OuterDimensionsJsonRoundTrip) {
      const OuterDimensionsMm dimensions{.width = 12.5, .height = 40.0, .depth = 12.5};
      nlohmann::json json = dimensions;
      const auto restored = json.get<OuterDimensionsMm>();
      EXPECT_EQ(restored, dimensions);
    }

    TEST(BoxTypesTest, ContainerTypeJsonRoundTripPreservesOptionalDimensions) {
      const ContainerType container_type{
          .id = id_from_low<ContainerTypeId>(1),
          .lab_id = id_from_low<LabId>(2),
          .name = "2 mL cryovial",
          .size_class = "cryovial_2ml",
          .outer_dimensions_mm = OuterDimensionsMm{.width = 12.5, .height = 40.0, .depth = 12.5},
          .material = "polypropylene",
          .supplier_sku = "CV-2",
          .created_at = Timestamp::from_unix_micros(100),
      };

      nlohmann::json json = container_type;
      const auto restored = json.get<ContainerType>();
      EXPECT_EQ(restored, container_type);
    }

    TEST(BoxTypesTest, ContainerTypeJsonRoundTripWithNullDimensions) {
      const ContainerType container_type{
          .id = id_from_low<ContainerTypeId>(3),
          .lab_id = id_from_low<LabId>(4),
          .name = "Unknown tube",
          .size_class = "tube_unknown",
          .created_at = Timestamp::from_unix_micros(200),
      };

      nlohmann::json json = container_type;
      EXPECT_TRUE(json.at("outer_dimensions_mm").is_null());
      const auto restored = json.get<ContainerType>();
      EXPECT_EQ(restored, container_type);
    }

    TEST(BoxTypesTest, BoxTypeJsonRoundTripPreservesPositionsAndAccepts) {
      const BoxType box_type{
          .id = id_from_low<BoxTypeId>(5),
          .lab_id = id_from_low<LabId>(6),
          .name = "Mixed rack",
          .manufacturer = "Eppendorf",
          .sku = "mixed",
          .positions =
              std::vector<Position>{
                  Position{
                      .label = "A1",
                      .row = 0,
                      .col = 0,
                      .accepts = {"falcon_50ml"},
                  },
                  Position{
                      .label = "B1",
                      .row = 1,
                      .col = 0,
                      .z = 1,
                      .accepts = {"falcon_15ml", "cryovial_2ml"},
                  },
              },
          .created_at = Timestamp::from_unix_micros(300),
      };

      nlohmann::json json = box_type;
      const auto restored = json.get<BoxType>();
      EXPECT_EQ(restored, box_type);
    }

  } // namespace
} // namespace fmgr::core
