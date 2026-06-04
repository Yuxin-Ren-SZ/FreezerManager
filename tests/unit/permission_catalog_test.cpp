// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/permissions.h"

#include <gtest/gtest.h>

#include <set>
#include <stdexcept>
#include <string>

namespace fmgr::core {
  namespace {

    TEST(PermissionCatalog, EveryEnumValueRoundTripsThroughKey) {
      const auto all = all_permissions();
      ASSERT_FALSE(all.empty());
      std::set<std::string> seen_keys;
      for (const auto permission : all) {
        const auto key = std::string(to_key(permission));
        EXPECT_TRUE(seen_keys.insert(key).second) << "duplicate permission key " << key;
        EXPECT_EQ(parse_permission(key), permission);
      }
    }

    TEST(PermissionCatalog, ParseRejectsUnknownKey) {
      EXPECT_THROW((void)parse_permission("not.a.real.permission"), std::invalid_argument);
    }

    TEST(PermissionCatalog, KeysFollowDottedNamespaceConvention) {
      for (const auto permission : all_permissions()) {
        const auto key = std::string(to_key(permission));
        EXPECT_NE(key.find('.'), std::string::npos)
            << "permission " << key << " must namespace its action";
      }
    }

    TEST(PermissionCatalog, BuiltinRolesGrantNonEmptyDisjointSubsets) {
      const auto admin = builtin_role_permissions(RoleKind::SystemAdmin);
      const auto lab_admin = builtin_role_permissions(RoleKind::LabAdmin);
      const auto member = builtin_role_permissions(RoleKind::Member);
      const auto read_only = builtin_role_permissions(RoleKind::ReadOnly);
      const auto api_client = builtin_role_permissions(RoleKind::ApiClient);

      EXPECT_FALSE(admin.empty());
      EXPECT_FALSE(lab_admin.empty());
      EXPECT_FALSE(member.empty());
      EXPECT_FALSE(read_only.empty());
      EXPECT_FALSE(api_client.empty());

      EXPECT_TRUE(read_only.contains(Permission::SampleRead));
      EXPECT_FALSE(read_only.contains(Permission::SampleWrite));
      EXPECT_TRUE(member.contains(Permission::SampleWrite));
      EXPECT_TRUE(lab_admin.contains(Permission::UserInvite));
      EXPECT_TRUE(admin.contains(Permission::SampleDeleteHard));
      EXPECT_FALSE(member.contains(Permission::SampleDeleteHard));
    }

    TEST(PermissionCatalog, GlobalOnlyPermissionsAreExplicitlyClassified) {
      EXPECT_TRUE(is_global_only_permission(Permission::SampleDeleteHard));
      EXPECT_TRUE(is_global_only_permission(Permission::BackupRun));
      EXPECT_TRUE(is_global_only_permission(Permission::KeyRotate));

      EXPECT_FALSE(is_global_only_permission(Permission::SampleRead));
      EXPECT_FALSE(is_global_only_permission(Permission::AuditExport));
      EXPECT_FALSE(is_global_only_permission(Permission::SessionRevoke));
    }

    TEST(PermissionCatalog, LabAdminDoesNotGrantDeploymentWideBackupRun) {
      const auto lab_admin = builtin_role_permissions(RoleKind::LabAdmin);
      EXPECT_FALSE(lab_admin.contains(Permission::BackupRun));
    }

  } // namespace
} // namespace fmgr::core
