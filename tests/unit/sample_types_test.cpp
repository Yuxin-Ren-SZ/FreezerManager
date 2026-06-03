// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/sample.h"

#include "test_helpers.h"
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <map>

namespace fmgr::core {
  namespace {
    using namespace fmgr::test;

    [[nodiscard]] Uuid uuid_from_low(std::uint64_t low_bits) {
      std::array<std::uint8_t, 16> bytes{};
      for (std::size_t i = 0; i < 8; ++i) {
        bytes.at(15 - i) = static_cast<std::uint8_t>((low_bits >> (i * 8U)) & 0xffU);
      }
      return Uuid(bytes);
    }

    template <typename StrongId> [[nodiscard]] StrongId id_from_low(std::uint64_t low_bits) {
      return StrongId(uuid_from_low(low_bits));
    }

    [[nodiscard]] Timestamp ts(std::int64_t micros) {
      return Timestamp::from_unix_micros(micros);
    }

    // ---- VolumeUnit / MassUnit JSON converters ----

    TEST(VolumeUnitJson, RoundTrip) {
      const nlohmann::json json_ml = VolumeUnit::Milliliter;
      const nlohmann::json json_ul = VolumeUnit::Microliter;
      EXPECT_EQ(json_ml.get<std::string>(), "mL");
      EXPECT_EQ(json_ul.get<std::string>(), "µL");
      EXPECT_EQ(json_ml.get<VolumeUnit>(), VolumeUnit::Milliliter);
      EXPECT_EQ(json_ul.get<VolumeUnit>(), VolumeUnit::Microliter);
    }

    TEST(MassUnitJson, RoundTrip) {
      const nlohmann::json json_mg = MassUnit::Milligram;
      const nlohmann::json json_g = MassUnit::Gram;
      EXPECT_EQ(json_mg.get<std::string>(), "mg");
      EXPECT_EQ(json_g.get<std::string>(), "g");
      EXPECT_EQ(json_mg.get<MassUnit>(), MassUnit::Milligram);
      EXPECT_EQ(json_g.get<MassUnit>(), MassUnit::Gram);
    }

    // ---- Project ----

    TEST(ProjectJson, RoundTripAllFields) {
      const Project project{
          .id = id_from_low<ProjectId>(1),
          .lab_id = id_from_low<LabId>(2),
          .name = "Tumor Bank 2026",
          .owner_user_id = id_from_low<UserId>(3),
          .created_at = ts(1000),
          .archived_at = std::nullopt,
      };
      const nlohmann::json json = project;
      const Project round_tripped = json.get<Project>();
      EXPECT_EQ(round_tripped, project);
    }

    TEST(ProjectJson, RoundTripWithArchivedAt) {
      const Project project{
          .id = id_from_low<ProjectId>(10),
          .lab_id = id_from_low<LabId>(11),
          .name = "Old Study",
          .owner_user_id = id_from_low<UserId>(12),
          .created_at = ts(500),
          .archived_at = ts(9999),
      };
      const nlohmann::json json = project;
      const Project round_tripped = json.get<Project>();
      EXPECT_EQ(round_tripped, project);
    }

    // ---- SampleProjectId comparison ----

    TEST(SampleProjectId, OrderingForMapKey) {
      const SampleProjectId id1{id_from_low<SampleId>(1), id_from_low<ProjectId>(1)};
      const SampleProjectId id2{id_from_low<SampleId>(1), id_from_low<ProjectId>(2)};
      const SampleProjectId id3{id_from_low<SampleId>(2), id_from_low<ProjectId>(1)};
      // Verify std::map accepts SampleProjectId as key (requires strict weak ordering).
      std::map<SampleProjectId, int> map;
      map[id1] = 1;
      map[id2] = 2;
      map[id3] = 3;
      EXPECT_EQ(map.at(id1), 1);
      EXPECT_EQ(map.at(id2), 2);
      EXPECT_EQ(map.at(id3), 3);
      EXPECT_TRUE(id1 < id2 || id2 < id1); // distinct keys are ordered
      EXPECT_FALSE(id1 < id1);             // reflexive irreflexivity
    }

    // ---- SampleProject ----

    TEST(SampleProjectJson, RoundTrip) {
      const SampleProject sp{
          .sample_id = id_from_low<SampleId>(5),
          .project_id = id_from_low<ProjectId>(6),
      };
      const nlohmann::json json = sp;
      const SampleProject round_tripped = json.get<SampleProject>();
      EXPECT_EQ(round_tripped, sp);
    }

    TEST(SampleProject, IdMethodReturnsCompositeKey) {
      const SampleProject sp{
          .sample_id = id_from_low<SampleId>(7),
          .project_id = id_from_low<ProjectId>(8),
      };
      const SampleProjectId sp_id = sp.id();
      EXPECT_EQ(sp_id.sample_id, sp.sample_id);
      EXPECT_EQ(sp_id.project_id, sp.project_id);
    }

    // ---- CheckoutEvent ----

    TEST(CheckoutEventJson, RoundTripFullEvent) {
      const CheckoutEvent event{
          .id = id_from_low<CheckoutEventId>(20),
          .sample_id = id_from_low<SampleId>(21),
          .lab_id = id_from_low<LabId>(22),
          .user_id = id_from_low<UserId>(23),
          .action = CheckoutAction::CheckedOut,
          .reason = "bench experiment",
          .at = ts(777000),
          .volume_delta = -500,
          .volume_unit = VolumeUnit::Microliter,
          .location_after = "Bench 3, Rack A",
      };
      const nlohmann::json json = event;
      const CheckoutEvent round_tripped = json.get<CheckoutEvent>();
      EXPECT_EQ(round_tripped, event);
    }

    TEST(CheckoutEventJson, RoundTripNullableFieldsNull) {
      const CheckoutEvent event{
          .id = id_from_low<CheckoutEventId>(30),
          .sample_id = id_from_low<SampleId>(31),
          .lab_id = id_from_low<LabId>(32),
          .user_id = id_from_low<UserId>(33),
          .action = CheckoutAction::Destroyed,
          .reason = std::nullopt,
          .at = ts(1),
          .volume_delta = std::nullopt,
          .volume_unit = std::nullopt,
          .location_after = std::nullopt,
      };
      const nlohmann::json json = event;
      const CheckoutEvent round_tripped = json.get<CheckoutEvent>();
      EXPECT_EQ(round_tripped, event);
    }

    // ---- Sample ----

    TEST(SampleJson, RoundTripFullySaturated) {
      const Sample sample{
          .id = id_from_low<SampleId>(100),
          .lab_id = id_from_low<LabId>(101),
          .item_type_id = id_from_low<ItemTypeId>(102),
          .name = "Serum aliquot #7",
          .barcode = "BC-9001",
          .container_type_id = id_from_low<ContainerTypeId>(103),
          .box_id = id_from_low<BoxId>(104),
          .position_label = "B3",
          .volume_value = 250,
          .volume_unit = VolumeUnit::Microliter,
          .mass_value = std::nullopt,
          .mass_unit = std::nullopt,
          .status = SampleStatus::Active,
          .parent_sample_id = id_from_low<SampleId>(99),
          .created_by = id_from_low<UserId>(200),
          .created_at = ts(5000),
          .last_modified_by = id_from_low<UserId>(200),
          .last_modified_at = ts(6000),
          .custom_fields_json = R"({"patient_id":"P001"})",
          .phi_fields_enc_json = "{}",
      };
      const nlohmann::json json = sample;
      const Sample round_tripped = json.get<Sample>();
      EXPECT_EQ(round_tripped, sample);
    }

    TEST(SampleJson, RoundTripMinimalNullables) {
      const Sample sample{
          .id = id_from_low<SampleId>(200),
          .lab_id = id_from_low<LabId>(201),
          .item_type_id = id_from_low<ItemTypeId>(202),
          .name = "Powder",
          .barcode = std::nullopt,
          .container_type_id = std::nullopt,
          .box_id = std::nullopt,
          .position_label = std::nullopt,
          .volume_value = std::nullopt,
          .volume_unit = std::nullopt,
          .mass_value = 10,
          .mass_unit = MassUnit::Milligram,
          .status = SampleStatus::Depleted,
          .parent_sample_id = std::nullopt,
          .created_by = id_from_low<UserId>(300),
          .created_at = ts(1),
          .last_modified_by = id_from_low<UserId>(300),
          .last_modified_at = ts(2),
          .custom_fields_json = "{}",
          .phi_fields_enc_json = "{}",
      };
      const nlohmann::json json = sample;
      const Sample round_tripped = json.get<Sample>();
      EXPECT_EQ(round_tripped, sample);
    }

    TEST(SampleJson, TombstonedStatusRoundTrips) {
      Sample sample{};
      sample.id = id_from_low<SampleId>(300);
      sample.lab_id = id_from_low<LabId>(301);
      sample.item_type_id = id_from_low<ItemTypeId>(302);
      sample.name = "archived";
      sample.status = SampleStatus::Tombstoned;
      sample.created_by = id_from_low<UserId>(400);
      sample.created_at = ts(1);
      sample.last_modified_by = id_from_low<UserId>(400);
      sample.last_modified_at = ts(2);
      const nlohmann::json json = sample;
      EXPECT_EQ(json.at("status").get<std::string>(), "tombstoned");
      EXPECT_EQ(json.get<Sample>().status, SampleStatus::Tombstoned);
    }

  } // namespace
} // namespace fmgr::core
