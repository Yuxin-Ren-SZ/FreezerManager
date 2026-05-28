// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/item_type.h"

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

    TEST(ScopeKindTest, StringRoundTrip) {
      for (const auto kind :
           {ScopeKind::Sample, ScopeKind::Box, ScopeKind::Freezer, ScopeKind::Container}) {
        EXPECT_EQ(parse_scope_kind(to_string(kind)), kind);
      }
    }

    TEST(ScopeKindTest, JsonRoundTrip) {
      for (const auto kind :
           {ScopeKind::Sample, ScopeKind::Box, ScopeKind::Freezer, ScopeKind::Container}) {
        nlohmann::json json = kind;
        EXPECT_EQ(json.get<ScopeKind>(), kind);
      }
    }

    TEST(FieldDataTypeTest, StringRoundTrip) {
      for (const auto dt : {FieldDataType::String, FieldDataType::Int, FieldDataType::Float,
                            FieldDataType::Bool, FieldDataType::Date, FieldDataType::Datetime,
                            FieldDataType::Enum, FieldDataType::Reference}) {
        EXPECT_EQ(parse_field_data_type(to_string(dt)), dt);
      }
    }

    TEST(FieldDataTypeTest, JsonRoundTrip) {
      for (const auto dt : {FieldDataType::String, FieldDataType::Int, FieldDataType::Float,
                            FieldDataType::Bool, FieldDataType::Date, FieldDataType::Datetime,
                            FieldDataType::Enum, FieldDataType::Reference}) {
        nlohmann::json json = dt;
        EXPECT_EQ(json.get<FieldDataType>(), dt);
      }
    }

    TEST(ItemTypeTest, JsonRoundTripWithParentAndArchivedAt) {
      const ItemType item_type{
          .id = id_from_low<ItemTypeId>(1),
          .lab_id = id_from_low<LabId>(2),
          .parent_id = id_from_low<ItemTypeId>(3),
          .name = "blood",
          .created_at = Timestamp::from_unix_micros(1000),
          .archived_at = Timestamp::from_unix_micros(2000),
      };
      const nlohmann::json json = item_type;
      const auto restored = json.get<ItemType>();
      EXPECT_EQ(restored, item_type);
    }

    TEST(ItemTypeTest, JsonRoundTripRootNodeNoParentNoArchivedAt) {
      const ItemType item_type{
          .id = id_from_low<ItemTypeId>(10),
          .lab_id = id_from_low<LabId>(11),
          .parent_id = std::nullopt,
          .name = "liquid",
          .created_at = Timestamp::from_unix_micros(500),
      };
      const nlohmann::json json = item_type;
      EXPECT_TRUE(json.at("parent_id").is_null());
      EXPECT_TRUE(json.at("archived_at").is_null());
      const auto restored = json.get<ItemType>();
      EXPECT_EQ(restored, item_type);
    }

    TEST(CustomFieldDefinitionTest, JsonRoundTripWithItemTypeIdAndValidationJson) {
      const CustomFieldDefinition cfd{
          .id = id_from_low<CustomFieldDefinitionId>(20),
          .lab_id = id_from_low<LabId>(21),
          .scope_kind = ScopeKind::Sample,
          .item_type_id = id_from_low<ItemTypeId>(22),
          .key = "patient_id",
          .label = "Patient ID",
          .data_type = FieldDataType::String,
          .required = true,
          .validation_json = R"({"max_length":64})",
          .indexed = false,
          .is_phi = true,
          .created_at = Timestamp::from_unix_micros(3000),
      };
      const nlohmann::json json = cfd;
      const auto restored = json.get<CustomFieldDefinition>();
      EXPECT_EQ(restored, cfd);
    }

    TEST(CustomFieldDefinitionTest, JsonRoundTripGlobalScopeNoItemTypeId) {
      const CustomFieldDefinition cfd{
          .id = id_from_low<CustomFieldDefinitionId>(30),
          .lab_id = id_from_low<LabId>(31),
          .scope_kind = ScopeKind::Box,
          .item_type_id = std::nullopt,
          .key = "fill_date",
          .label = "Fill Date",
          .data_type = FieldDataType::Date,
          .required = false,
          .validation_json = "{}",
          .indexed = false,
          .is_phi = false,
          .created_at = Timestamp::from_unix_micros(4000),
      };
      const nlohmann::json json = cfd;
      EXPECT_TRUE(json.at("item_type_id").is_null());
      EXPECT_TRUE(json.at("archived_at").is_null());
      const auto restored = json.get<CustomFieldDefinition>();
      EXPECT_EQ(restored, cfd);
    }

  } // namespace
} // namespace fmgr::core
