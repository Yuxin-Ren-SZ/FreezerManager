// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/role.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <stdexcept>

namespace fmgr::core {
  namespace {

    TEST(RoleTypes, BuiltinRoleIdsAreStableAndDistinct) {
      const auto admin = builtin_role_id(RoleKind::SystemAdmin);
      const auto lab_admin = builtin_role_id(RoleKind::LabAdmin);
      const auto member = builtin_role_id(RoleKind::Member);
      const auto read_only = builtin_role_id(RoleKind::ReadOnly);
      const auto api_client = builtin_role_id(RoleKind::ApiClient);

      EXPECT_NE(admin, lab_admin);
      EXPECT_NE(lab_admin, member);
      EXPECT_NE(member, read_only);
      EXPECT_NE(read_only, api_client);

      // Stability: the IDs are documented in code and the migration relies
      // on them. If this assertion changes, the seed migration must change.
      EXPECT_EQ(admin.to_string(), "00000000-0000-0000-0000-000000000001");
    }

    TEST(RoleTypes, RoleRoundTripsThroughJson) {
      const Role role{
          .id = builtin_role_id(RoleKind::SystemAdmin),
          .lab_id = std::nullopt,
          .kind = RoleKind::SystemAdmin,
          .name = "SystemAdmin",
          .description = "Deployment-wide administrator",
          .is_builtin = true,
          .created_at = Timestamp::from_unix_micros(1'700'000'000'000'000),
          .archived_at = std::nullopt,
      };
      const nlohmann::json json = role;
      EXPECT_EQ(json.get<Role>(), role);
    }

    TEST(RoleTypes, RolePermissionRoundTripsThroughJson) {
      const RolePermission grant{
          .role_id = builtin_role_id(RoleKind::Member),
          .permission = Permission::SampleWrite,
      };
      const nlohmann::json json = grant;
      EXPECT_EQ(json.get<RolePermission>(), grant);
      EXPECT_EQ(grant.id().to_string(),
                builtin_role_id(RoleKind::Member).to_string() + ":sample.write");
    }

    TEST(ScopeFilter, AcceptsEmptyObjectAndKnownKeys) {
      EXPECT_NO_THROW(validate_scope_filter(nlohmann::json::object()));
      EXPECT_NO_THROW(
          validate_scope_filter(nlohmann::json{{"freezer_in", nlohmann::json::array({"a", "b"})}}));
      EXPECT_NO_THROW(validate_scope_filter(nlohmann::json{
          {"freezer_in", nlohmann::json::array()},
          {"project_in", nlohmann::json::array({"p1"})},
          {"item_type_in", nlohmann::json::array({"t1"})},
      }));
    }

    TEST(ScopeFilter, RejectsNonObjectsAndUnknownKeysAndNonStringValues) {
      EXPECT_THROW(validate_scope_filter(nlohmann::json::array()), std::invalid_argument);
      EXPECT_THROW(validate_scope_filter(nlohmann::json{{"unknown_key", nlohmann::json::array()}}),
                   std::invalid_argument);
      EXPECT_THROW(validate_scope_filter(nlohmann::json{{"freezer_in", "not-an-array"}}),
                   std::invalid_argument);
      EXPECT_THROW(validate_scope_filter(nlohmann::json{
                       {"freezer_in", nlohmann::json::array({1, 2})},
                   }),
                   std::invalid_argument);
    }

  } // namespace
} // namespace fmgr::core
