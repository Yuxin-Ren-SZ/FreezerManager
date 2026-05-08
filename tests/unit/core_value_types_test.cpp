// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/enums.h"
#include "core/ids.h"
#include "core/quantity.h"
#include "core/timestamp.h"
#include "core/uuid.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <concepts>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace fmgr::core {
  namespace {

    TEST(CoreUuid, ParsesAndFormatsCanonicalUuid) {
      const auto uuid = Uuid::parse("550E8400-E29B-41D4-A716-446655440000");

      EXPECT_EQ(uuid.to_string(), "550e8400-e29b-41d4-a716-446655440000");
    }

    TEST(CoreUuid, RejectsInvalidUuidText) {
      EXPECT_THROW((void)Uuid::parse("not-a-uuid"), std::invalid_argument);
      EXPECT_THROW((void)Uuid::parse("550e8400e29b41d4a716446655440000"), std::invalid_argument);
      EXPECT_THROW((void)Uuid::parse("550e8400-e29b-41d4-a716-44665544000z"),
                   std::invalid_argument);
    }

    TEST(CoreIds, AreStronglyTypedAndRoundTripThroughJson) {
      static_assert(!std::is_convertible_v<LabId, UserId>);
      static_assert(!std::constructible_from<LabId, UserId>);

      const auto lab_id = LabId::parse("550e8400-e29b-41d4-a716-446655440000");
      const auto same_lab_id = LabId::parse("550e8400-e29b-41d4-a716-446655440000");
      const auto other_lab_id = LabId::parse("650e8400-e29b-41d4-a716-446655440000");

      EXPECT_EQ(lab_id, same_lab_id);
      EXPECT_LT(lab_id, other_lab_id);

      const nlohmann::json json = lab_id;
      EXPECT_EQ(json.get<std::string>(), "550e8400-e29b-41d4-a716-446655440000");
      EXPECT_EQ(json.get<LabId>(), lab_id);
    }

    TEST(CoreQuantity, VolumeSupportsSameUnitArithmeticAndRejectsMixedUnits) {
      const auto left = Volume::from_microunits(1'500'000, VolumeUnit::Milliliter);
      const auto right = Volume::from_microunits(250'000, VolumeUnit::Milliliter);
      const auto microliters = Volume::from_microunits(250'000, VolumeUnit::Microliter);

      EXPECT_EQ((left + right).value_microunits(), 1'750'000);
      EXPECT_EQ((left - right).value_microunits(), 1'250'000);
      EXPECT_GT(left, right);
      EXPECT_THROW((void)(left + microliters), std::invalid_argument);
      EXPECT_THROW((void)(left < microliters), std::invalid_argument);
    }

    TEST(CoreQuantity, MassSupportsSameUnitArithmeticAndRejectsMixedUnits) {
      const auto left = Mass::from_microunits(2'000'000, MassUnit::Gram);
      const auto right = Mass::from_microunits(500'000, MassUnit::Gram);
      const auto milligrams = Mass::from_microunits(500'000, MassUnit::Milligram);

      EXPECT_EQ((left + right).value_microunits(), 2'500'000);
      EXPECT_GT(left, right);
      EXPECT_THROW((void)(left + milligrams), std::invalid_argument);
    }

    TEST(CoreQuantity, QuantityJsonRoundTrips) {
      const auto volume = Volume::from_microunits(42, VolumeUnit::Microliter);
      const nlohmann::json volume_json = volume;

      EXPECT_EQ(volume_json.at("value_microunits").get<std::int64_t>(), 42);
      EXPECT_EQ(volume_json.at("unit").get<std::string>(), "uL");
      EXPECT_EQ(volume_json.get<Volume>(), volume);

      const auto mass = Mass::from_microunits(99, MassUnit::Milligram);
      const nlohmann::json mass_json = mass;

      EXPECT_EQ(mass_json.at("unit").get<std::string>(), "mg");
      EXPECT_EQ(mass_json.get<Mass>(), mass);
    }

    TEST(CoreTimestamp, PreservesUtcUnixMicroseconds) {
      const auto timestamp = Timestamp::from_unix_micros(1'799'000'123'456'789);
      const auto later = Timestamp::from_unix_micros(1'799'000'123'456'790);

      EXPECT_LT(timestamp, later);

      const nlohmann::json json = timestamp;
      EXPECT_EQ(json.get<std::int64_t>(), 1'799'000'123'456'789);
      EXPECT_EQ(json.get<Timestamp>(), timestamp);
    }

    TEST(CoreEnums, StringConversionsAndJsonRoundTrip) {
      EXPECT_EQ(to_string(SampleStatus::CheckedOut), "checked_out");
      EXPECT_EQ(parse_sample_status("depleted"), SampleStatus::Depleted);
      EXPECT_THROW((void)parse_sample_status("missing"), std::invalid_argument);

      const nlohmann::json sample_status_json = SampleStatus::Active;
      EXPECT_EQ(sample_status_json.get<std::string>(), "active");
      EXPECT_EQ(sample_status_json.get<SampleStatus>(), SampleStatus::Active);

      const nlohmann::json checkout_action_json = CheckoutAction::Destroyed;
      EXPECT_EQ(checkout_action_json.get<std::string>(), "destroy");
      EXPECT_EQ(checkout_action_json.get<CheckoutAction>(), CheckoutAction::Destroyed);

      const nlohmann::json role_kind_json = RoleKind::SystemAdmin;
      EXPECT_EQ(role_kind_json.get<std::string>(), "system_admin");
      EXPECT_EQ(role_kind_json.get<RoleKind>(), RoleKind::SystemAdmin);

      const nlohmann::json container_kind_json = ContainerKind::Compartment;
      EXPECT_EQ(container_kind_json.get<std::string>(), "compartment");
      EXPECT_EQ(container_kind_json.get<ContainerKind>(), ContainerKind::Compartment);
    }

  } // namespace
} // namespace fmgr::core
