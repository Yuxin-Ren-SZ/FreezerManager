// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/identity.h"
#include "core/permissions.h"
#include "core/role.h"
#include "storage/IdentityTraits.h"
#include "storage/RoleTraits.h"
#include "storage/sqlite/IdentityRepositories.h"
#include "storage/sqlite/RoleRepositories.h"
#include "storage/sqlite/SqliteBackend.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>

namespace fmgr::storage {
  namespace {

    [[nodiscard]] core::Uuid uuid_from_low(std::uint64_t low_bits) {
      std::array<std::uint8_t, 16> bytes{};
      for (std::size_t index = 0; index < 8; ++index) {
        bytes.at(15 - index) = static_cast<std::uint8_t>((low_bits >> (index * 8U)) & 0xffU);
      }
      return core::Uuid(bytes);
    }

    template <typename StrongId> [[nodiscard]] StrongId id_from_low(std::uint64_t low_bits) {
      return StrongId(uuid_from_low(low_bits));
    }

    [[nodiscard]] std::filesystem::path sqlite_test_path(std::string_view suffix) {
      const auto unique = std::to_string(static_cast<unsigned long long>(
                              ::testing::UnitTest::GetInstance()->random_seed())) +
                          "-" + std::to_string(reinterpret_cast<std::uintptr_t>(&suffix));
      return std::filesystem::temp_directory_path() / (std::string("freezermanager-sqlite-roles-") +
                                                       unique + "-" + std::string(suffix) + ".db");
    }

    void remove_sqlite_files(const std::filesystem::path& path) {
      std::filesystem::remove(path);
      std::filesystem::remove(path.string() + "-wal");
      std::filesystem::remove(path.string() + "-shm");
    }

    [[nodiscard]] MutationContext mutation_context() {
      return MutationContext{
          .actor_user_id = id_from_low<core::UserId>(900),
          .actor_session_id = "role-session",
          .request_id = "role-request",
          .reason = "role repository test",
      };
    }

    class SqliteRoleRepositoryTest : public ::testing::Test {
    protected:
      SqliteRoleRepositoryTest()
          : db_path_(sqlite_test_path("repositories")), backend_(SqliteBackendOptions{
                                                            .database_path = db_path_.string(),
                                                        }) {
        register_identity_repositories(backend_);
        register_role_repositories(backend_);
      }

      void SetUp() override {
        remove_sqlite_files(db_path_);
        backend_.migrate_to_latest();
      }

      void TearDown() override {
        remove_sqlite_files(db_path_);
      }

      [[nodiscard]] SqliteBackend& backend() {
        return backend_;
      }

    private:
      std::filesystem::path db_path_;
      SqliteBackend backend_;
    };

    void assert_role_archived(IRepository<core::Role>& roles, const core::RoleId& role_id) {
      const auto active = roles.query(Query<core::Role>::all());
      for (const auto& candidate : active) {
        EXPECT_NE(candidate.id, role_id) << "archived role should not appear in default query";
      }
      const auto all = roles.query(Query<core::Role>::all().include_tombstoned());
      bool found_archived = false;
      for (const auto& candidate : all) {
        if (candidate.id == role_id) {
          EXPECT_TRUE(candidate.archived_at.has_value());
          found_archived = true;
        }
      }
      EXPECT_TRUE(found_archived);
    }

    void verify_seeded_role(IRepository<core::Role>& roles,
                            IRepository<core::RolePermission>& grants, core::RoleKind kind) {
      const auto role = roles.find_by_id(core::builtin_role_id(kind));
      ASSERT_TRUE(role.has_value()) << core::to_string(kind);
      // NOLINTBEGIN(bugprone-unchecked-optional-access)
      EXPECT_TRUE(role->is_builtin);
      EXPECT_FALSE(role->lab_id.has_value());
      EXPECT_EQ(role->kind, kind);

      const auto member_grants = grants.query(Query<core::RolePermission>::where(
          field<core::RolePermission, std::string>(core::RolePermission::Field::RoleId) ==
          role->id.to_string()));
      // NOLINTEND(bugprone-unchecked-optional-access)
      const auto expected = core::builtin_role_permissions(kind);
      std::set<core::Permission> actual;
      for (const auto& grant : member_grants) {
        actual.insert(grant.permission);
      }
      EXPECT_EQ(actual, expected) << "permission grants for " << core::to_string(kind);
    }

    TEST_F(SqliteRoleRepositoryTest, MigrationSeedsBuiltinRolesAndPermissionGrants) {
      auto transaction = backend().begin(IsolationLevel::Serializable);
      auto& roles = transaction->repo<core::Role>();
      auto& grants = transaction->repo<core::RolePermission>();

      const auto stored_roles = roles.query(Query<core::Role>::all());
      ASSERT_EQ(stored_roles.size(), 5U);

      for (const auto kind :
           {core::RoleKind::SystemAdmin, core::RoleKind::LabAdmin, core::RoleKind::Member,
            core::RoleKind::ReadOnly, core::RoleKind::ApiClient}) {
        verify_seeded_role(roles, grants, kind);
      }
    }

    TEST_F(SqliteRoleRepositoryTest, CustomRoleInsertAndArchiveRoundTrip) {
      const core::Lab lab_entity{
          .id = id_from_low<core::LabId>(700),
          .name = "Custom Lab",
          .contact = "lab@example.org",
          .created_at = core::Timestamp::from_unix_micros(1),
          .settings_json = nlohmann::json::object(),
      };
      auto seed = backend().begin(IsolationLevel::Serializable);
      seed->repo<core::Lab>().insert(lab_entity, mutation_context());
      seed->commit();

      const core::Role role{
          .id = id_from_low<core::RoleId>(701),
          .lab_id = lab_entity.id,
          .kind = core::RoleKind::Member,
          .name = "Bench Tech",
          .description = "Lab-scoped tech role",
          .is_builtin = false,
          .created_at = core::Timestamp::from_unix_micros(10),
      };

      auto transaction = backend().begin(IsolationLevel::Serializable);
      transaction->repo<core::Role>().insert(role, mutation_context());
      transaction->repo<core::RolePermission>().insert(
          core::RolePermission{.role_id = role.id, .permission = core::Permission::SampleRead},
          mutation_context());
      transaction->commit();

      auto verify = backend().begin(IsolationLevel::Serializable);
      const auto stored = verify->repo<core::Role>().find_by_id(role.id);
      ASSERT_TRUE(stored.has_value());
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
      EXPECT_EQ(stored->name, "Bench Tech");
      verify->repo<core::Role>().soft_delete(role.id, mutation_context());
      verify->commit();

      auto check = backend().begin(IsolationLevel::Serializable);
      assert_role_archived(check->repo<core::Role>(), role.id);
    }

    TEST_F(SqliteRoleRepositoryTest, ArchivingBuiltinRoleIsRejected) {
      auto transaction = backend().begin(IsolationLevel::Serializable);
      EXPECT_THROW(transaction->repo<core::Role>().soft_delete(
                       core::builtin_role_id(core::RoleKind::SystemAdmin), mutation_context()),
                   ConstraintViolation);
    }

    TEST_F(SqliteRoleRepositoryTest, GrantingPermissionToMissingRoleIsForeignKeyViolation) {
      auto transaction = backend().begin(IsolationLevel::Serializable);
      transaction->repo<core::RolePermission>().insert(
          core::RolePermission{
              .role_id = id_from_low<core::RoleId>(9999),
              .permission = core::Permission::SampleRead,
          },
          mutation_context());

      EXPECT_THROW(transaction->commit(), ForeignKeyViolation);
    }

    TEST_F(SqliteRoleRepositoryTest, RevokingPermissionDeletesRow) {
      const auto role_id = core::builtin_role_id(core::RoleKind::Member);
      auto transaction = backend().begin(IsolationLevel::Serializable);
      transaction->repo<core::RolePermission>().soft_delete(
          core::RolePermissionId{.role_id = role_id, .permission = core::Permission::SampleWrite},
          mutation_context());
      transaction->commit();

      auto verify = backend().begin(IsolationLevel::Serializable);
      EXPECT_FALSE(verify->repo<core::RolePermission>()
                       .find_by_id(core::RolePermissionId{
                           .role_id = role_id, .permission = core::Permission::SampleWrite})
                       .has_value());
    }

    TEST_F(SqliteRoleRepositoryTest, LabMembershipPersistsRoleId) {
      const core::Lab lab_entity{
          .id = id_from_low<core::LabId>(800),
          .name = "Membership Lab",
          .contact = "x@example.org",
          .created_at = core::Timestamp::from_unix_micros(1),
          .settings_json = nlohmann::json::object(),
      };
      const core::User user_entity{
          .id = id_from_low<core::UserId>(801),
          .primary_email = "tester@example.org",
          .display_name = "Tester",
          .status = core::UserStatus::Active,
          .created_at = core::Timestamp::from_unix_micros(2),
          .auth_bindings = nlohmann::json::array(),
      };
      const core::LabMembership membership{
          .user_id = user_entity.id,
          .lab_id = lab_entity.id,
          .role_id = core::builtin_role_id(core::RoleKind::Member),
          .scope_filters_json = nlohmann::json::object(),
          .joined_at = core::Timestamp::from_unix_micros(10),
      };

      auto transaction = backend().begin(IsolationLevel::Serializable);
      transaction->repo<core::Lab>().insert(lab_entity, mutation_context());
      transaction->repo<core::User>().insert(user_entity, mutation_context());
      transaction->repo<core::LabMembership>().insert(membership, mutation_context());
      transaction->commit();

      auto verify = backend().begin(IsolationLevel::Serializable);
      const auto stored = verify->repo<core::LabMembership>().find_by_id(membership.id());
      ASSERT_TRUE(stored.has_value());
      // NOLINTBEGIN(bugprone-unchecked-optional-access)
      ASSERT_TRUE(stored->role_id.has_value());
      EXPECT_EQ(stored->role_id.value(), core::builtin_role_id(core::RoleKind::Member));
      // NOLINTEND(bugprone-unchecked-optional-access)
    }

    TEST_F(SqliteRoleRepositoryTest, LabMembershipWithMissingRoleIsForeignKeyViolation) {
      const core::Lab lab_entity{
          .id = id_from_low<core::LabId>(810),
          .name = "FK Lab",
          .contact = "x@example.org",
          .created_at = core::Timestamp::from_unix_micros(1),
          .settings_json = nlohmann::json::object(),
      };
      const core::User user_entity{
          .id = id_from_low<core::UserId>(811),
          .primary_email = "fk@example.org",
          .display_name = "FK User",
          .status = core::UserStatus::Active,
          .created_at = core::Timestamp::from_unix_micros(2),
          .auth_bindings = nlohmann::json::array(),
      };
      const core::LabMembership membership{
          .user_id = user_entity.id,
          .lab_id = lab_entity.id,
          .role_id = id_from_low<core::RoleId>(9998),
          .scope_filters_json = nlohmann::json::object(),
          .joined_at = core::Timestamp::from_unix_micros(10),
      };

      auto transaction = backend().begin(IsolationLevel::Serializable);
      transaction->repo<core::Lab>().insert(lab_entity, mutation_context());
      transaction->repo<core::User>().insert(user_entity, mutation_context());
      transaction->repo<core::LabMembership>().insert(membership, mutation_context());

      EXPECT_THROW(transaction->commit(), ForeignKeyViolation);
    }

  } // namespace
} // namespace fmgr::storage
