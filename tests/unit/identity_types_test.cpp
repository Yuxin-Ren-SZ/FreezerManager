// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/identity.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>

namespace fmgr::core {
  namespace {

    TEST(IdentityTypes, UserStatusConvertsToStringsAndJson) {
      EXPECT_EQ(to_string(UserStatus::Active), "active");
      EXPECT_EQ(parse_user_status("disabled"), UserStatus::Disabled);
      EXPECT_THROW((void)parse_user_status("missing"), std::invalid_argument);

      const nlohmann::json json = UserStatus::Active;
      EXPECT_EQ(json.get<std::string>(), "active");
      EXPECT_EQ(json.get<UserStatus>(), UserStatus::Active);
    }

    TEST(IdentityTypes, LabMembershipIdIsCompositeUserAndLabId) {
      const auto user_id = UserId::parse("650e8400-e29b-41d4-a716-446655440000");
      const auto lab_id = LabId::parse("550e8400-e29b-41d4-a716-446655440000");
      const LabMembershipId membership_id{.user_id = user_id, .lab_id = lab_id};

      EXPECT_EQ(membership_id.to_string(), "650e8400-e29b-41d4-a716-446655440000:"
                                           "550e8400-e29b-41d4-a716-446655440000");
      EXPECT_EQ(LabMembershipId::parse(membership_id.to_string()), membership_id);
      EXPECT_THROW((void)LabMembershipId::parse(user_id.to_string()), std::invalid_argument);
    }

    TEST(IdentityTypes, IdentityEntitiesRoundTripThroughJson) {
      const auto created_at = Timestamp::from_unix_micros(1'799'000'123'456'789);
      const Lab lab{
          .id = LabId::parse("550e8400-e29b-41d4-a716-446655440000"),
          .name = "Neuro Bank",
          .contact = "admin@example.org",
          .created_at = created_at,
          .settings_json = nlohmann::json{{"timezone", "UTC"}},
          .is_phi_enabled = true,
      };
      const nlohmann::json lab_json = lab;
      EXPECT_EQ(lab_json.get<Lab>(), lab);

      const User user{
          .id = UserId::parse("650e8400-e29b-41d4-a716-446655440000"),
          .primary_email = "Ada@Example.ORG",
          .display_name = "Ada Lovelace",
          .status = UserStatus::Active,
          .created_at = created_at,
          .auth_bindings = nlohmann::json::array({{{"provider", "local"}}}),
          .default_lab_id = lab.id,
      };
      const nlohmann::json user_json = user;
      EXPECT_EQ(user_json.get<User>(), user);

      const LabMembership membership{
          .user_id = user.id,
          .lab_id = lab.id,
          .scope_filters_json = nlohmann::json::object(),
          .invited_by = user.id,
          .joined_at = created_at,
      };
      const nlohmann::json membership_json = membership;
      EXPECT_EQ(membership_json.get<LabMembership>(), membership);
    }

  } // namespace
} // namespace fmgr::core
