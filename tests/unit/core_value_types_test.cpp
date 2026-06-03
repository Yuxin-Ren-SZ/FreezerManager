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
      const auto left = Volume::from_raw(1'500'000, VolumeUnit::Milliliter);
      const auto right = Volume::from_raw(250'000, VolumeUnit::Milliliter);
      const auto microliters = Volume::from_raw(250'000, VolumeUnit::Microliter);

      EXPECT_EQ((left + right).raw_value(), 1'750'000);
      EXPECT_EQ((left - right).raw_value(), 1'250'000);
      EXPECT_GT(left, right);
      EXPECT_GE(left, right);
      EXPECT_LE(right, left);
      EXPECT_LT(right, left);
      EXPECT_THROW((void)(left + microliters), std::invalid_argument);
      EXPECT_THROW((void)(left < microliters), std::invalid_argument);
    }

    TEST(CoreQuantity, VolumeConvertsMilliliterToMicroliter) {
      const auto v = Volume::from_raw(1, VolumeUnit::Milliliter);
      const auto converted = v.to_unit(VolumeUnit::Microliter);
      EXPECT_EQ(converted.raw_value(), 1'000);
      EXPECT_EQ(converted.unit(), VolumeUnit::Microliter);
    }

    TEST(CoreQuantity, VolumeConvertsMicroliterToMilliliterTruncates) {
      const auto v = Volume::from_raw(500, VolumeUnit::Microliter);
      const auto converted = v.to_unit(VolumeUnit::Milliliter);
      EXPECT_EQ(converted.raw_value(), 0);
      EXPECT_EQ(converted.unit(), VolumeUnit::Milliliter);
    }

    TEST(CoreQuantity, VolumeSameUnitConversionIsNoop) {
      const auto v = Volume::from_raw(42, VolumeUnit::Microliter);
      const auto converted = v.to_unit(VolumeUnit::Microliter);
      EXPECT_EQ(converted.raw_value(), 42);
      EXPECT_EQ(converted.unit(), VolumeUnit::Microliter);
    }

    TEST(CoreQuantity, VolumeToUnitPreservesEquality) {
      const auto ml = Volume::from_raw(1, VolumeUnit::Milliliter);
      const auto ul = Volume::from_raw(1000, VolumeUnit::Microliter);
      EXPECT_EQ(ml.to_unit(VolumeUnit::Microliter), ul);
    }

    TEST(CoreQuantity, MassSupportsSameUnitArithmeticAndRejectsMixedUnits) {
      const auto left = Mass::from_raw(2'000'000, MassUnit::Gram);
      const auto right = Mass::from_raw(500'000, MassUnit::Gram);
      const auto milligrams = Mass::from_raw(500'000, MassUnit::Milligram);

      EXPECT_EQ((left + right).raw_value(), 2'500'000);
      EXPECT_GT(left, right);
      EXPECT_GE(left, right);
      EXPECT_LE(right, left);
      EXPECT_THROW((void)(left + milligrams), std::invalid_argument);
    }

    TEST(CoreQuantity, MassConvertsGramToMilligram) {
      const auto m = Mass::from_raw(1, MassUnit::Gram);
      const auto converted = m.to_unit(MassUnit::Milligram);
      EXPECT_EQ(converted.raw_value(), 1'000);
      EXPECT_EQ(converted.unit(), MassUnit::Milligram);
    }

    TEST(CoreQuantity, MassConvertsMilligramToGramTruncates) {
      const auto m = Mass::from_raw(500, MassUnit::Milligram);
      const auto converted = m.to_unit(MassUnit::Gram);
      EXPECT_EQ(converted.raw_value(), 0);
      EXPECT_EQ(converted.unit(), MassUnit::Gram);
    }

    TEST(CoreQuantity, MassSameUnitConversionIsNoop) {
      const auto m = Mass::from_raw(99, MassUnit::Milligram);
      const auto converted = m.to_unit(MassUnit::Milligram);
      EXPECT_EQ(converted.raw_value(), 99);
    }

    TEST(CoreQuantity, QuantityJsonRoundTrips) {
      const auto volume = Volume::from_raw(42, VolumeUnit::Microliter);
      const nlohmann::json volume_json = volume;

      EXPECT_EQ(volume_json.at("value").get<std::int64_t>(), 42);
      EXPECT_EQ(volume_json.at("unit").get<std::string>(), "µL");
      EXPECT_EQ(volume_json.get<Volume>(), volume);

      const auto mass = Mass::from_raw(99, MassUnit::Milligram);
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

    TEST(CoreEnums, AllSampleStatusVariantsRoundTrip) {
      using namespace fmgr::core;

      EXPECT_EQ(to_string(SampleStatus::Active), "active");
      EXPECT_EQ(to_string(SampleStatus::CheckedOut), "checked_out");
      EXPECT_EQ(to_string(SampleStatus::Depleted), "depleted");
      EXPECT_EQ(to_string(SampleStatus::Destroyed), "destroyed");
      EXPECT_EQ(to_string(SampleStatus::Tombstoned), "tombstoned");

      EXPECT_EQ(parse_sample_status("active"), SampleStatus::Active);
      EXPECT_EQ(parse_sample_status("checked_out"), SampleStatus::CheckedOut);
      EXPECT_EQ(parse_sample_status("depleted"), SampleStatus::Depleted);
      EXPECT_EQ(parse_sample_status("destroyed"), SampleStatus::Destroyed);
      EXPECT_EQ(parse_sample_status("tombstoned"), SampleStatus::Tombstoned);

      nlohmann::json j = SampleStatus::Active;
      EXPECT_EQ(j.get<std::string>(), "active");
      auto parsed = j.get<SampleStatus>();
      EXPECT_EQ(parsed, SampleStatus::Active);
    }

    TEST(CoreEnums, AllCheckoutActionVariantsRoundTrip) {
      using namespace fmgr::core;

      EXPECT_EQ(to_string(CheckoutAction::CheckedOut), "out");
      EXPECT_EQ(to_string(CheckoutAction::CheckedIn), "in");
      EXPECT_EQ(to_string(CheckoutAction::Destroyed), "destroy");

      EXPECT_EQ(parse_checkout_action("out"), CheckoutAction::CheckedOut);
      EXPECT_EQ(parse_checkout_action("in"), CheckoutAction::CheckedIn);
      EXPECT_EQ(parse_checkout_action("destroy"), CheckoutAction::Destroyed);

      nlohmann::json j = CheckoutAction::CheckedIn;
      EXPECT_EQ(j.get<std::string>(), "in");
      EXPECT_EQ(j.get<CheckoutAction>(), CheckoutAction::CheckedIn);
    }

    TEST(CoreEnums, AllRoleKindVariantsRoundTrip) {
      using namespace fmgr::core;

      EXPECT_EQ(to_string(RoleKind::SystemAdmin), "system_admin");
      EXPECT_EQ(to_string(RoleKind::LabAdmin), "lab_admin");
      EXPECT_EQ(to_string(RoleKind::Member), "member");
      EXPECT_EQ(to_string(RoleKind::ReadOnly), "read_only");
      EXPECT_EQ(to_string(RoleKind::ApiClient), "api_client");

      EXPECT_EQ(parse_role_kind("system_admin"), RoleKind::SystemAdmin);
      EXPECT_EQ(parse_role_kind("lab_admin"), RoleKind::LabAdmin);
      EXPECT_EQ(parse_role_kind("member"), RoleKind::Member);
      EXPECT_EQ(parse_role_kind("read_only"), RoleKind::ReadOnly);
      EXPECT_EQ(parse_role_kind("api_client"), RoleKind::ApiClient);

      nlohmann::json j = RoleKind::Member;
      EXPECT_EQ(j.get<std::string>(), "member");
      EXPECT_EQ(j.get<RoleKind>(), RoleKind::Member);
    }

    TEST(CoreEnums, AllContainerKindVariantsRoundTrip) {
      using namespace fmgr::core;

      EXPECT_EQ(to_string(ContainerKind::Compartment), "compartment");
      EXPECT_EQ(to_string(ContainerKind::Shelf), "shelf");
      EXPECT_EQ(to_string(ContainerKind::Rack), "rack");
      EXPECT_EQ(to_string(ContainerKind::Drawer), "drawer");
      EXPECT_EQ(to_string(ContainerKind::Custom), "custom");

      EXPECT_EQ(parse_container_kind("compartment"), ContainerKind::Compartment);
      EXPECT_EQ(parse_container_kind("shelf"), ContainerKind::Shelf);
      EXPECT_EQ(parse_container_kind("rack"), ContainerKind::Rack);
      EXPECT_EQ(parse_container_kind("drawer"), ContainerKind::Drawer);
      EXPECT_EQ(parse_container_kind("custom"), ContainerKind::Custom);

      nlohmann::json j = ContainerKind::Rack;
      EXPECT_EQ(j.get<std::string>(), "rack");
      EXPECT_EQ(j.get<ContainerKind>(), ContainerKind::Rack);
    }

    TEST(CoreEnums, UserStatusRoundTrip) {
      using namespace fmgr::core;

      EXPECT_EQ(to_string(UserStatus::Active), "active");
      EXPECT_EQ(to_string(UserStatus::Disabled), "disabled");

      EXPECT_EQ(parse_user_status("active"), UserStatus::Active);
      EXPECT_EQ(parse_user_status("disabled"), UserStatus::Disabled);

      nlohmann::json j = UserStatus::Active;
      EXPECT_EQ(j.get<std::string>(), "active");
      EXPECT_EQ(j.get<UserStatus>(), UserStatus::Active);
    }

    TEST(CoreEnums, UserStatusRejectsInvalidString) {
      using namespace fmgr::core;
      EXPECT_THROW(parse_user_status("invalid_status"), std::invalid_argument);
      EXPECT_THROW(parse_user_status(""), std::invalid_argument);
    }

    TEST(CoreEnums, RoleKindRejectsInvalidString) {
      using namespace fmgr::core;
      EXPECT_THROW(parse_role_kind("super_admin"), std::invalid_argument);
      EXPECT_THROW(parse_role_kind(""), std::invalid_argument);
    }

  } // namespace
} // namespace fmgr::core
