// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/audit_event.h"
#include "core/identity.h"
#include "core/permissions.h"
#include "core/role.h"
#include "storage/AuditTraits.h"
#include "storage/IdentityTraits.h"
#include "storage/RoleTraits.h"
#include "storage/postgres/AuditRepositories.h"
#include "storage/postgres/IdentityRepositories.h"
#include "storage/postgres/RoleRepositories.h"
#include "storage/sqlite/AuditRepositories.h"
#include "storage/sqlite/IdentityRepositories.h"
#include "storage/sqlite/RoleRepositories.h"

#include "repo_backend_harness.h"
#include "test_helpers.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <set>
#include <string>

namespace fmgr::storage {
  namespace {
    using namespace fmgr::test;

    [[nodiscard]] MutationContext mutation_context() {
      return MutationContext{
          .actor_user_id = id_from_low<core::UserId>(900),
          .actor_session_id = "role-session",
          .request_id = "role-request",
          .reason = "role repository test",
      };
    }

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

    class RoleRepositoryTest : public ::testing::TestWithParam<BackendKind> {
    protected:
      void SetUp() override {
        if (GetParam() == BackendKind::Postgres && !postgres_test_url().has_value()) {
          GTEST_SKIP() << "FMGR_TEST_POSTGRES_URL not set; skipping Postgres repository tests";
        }
        harness_ = std::make_unique<RepoBackendHarness>(
            GetParam(),
            [](SqliteBackend& backend) {
              register_identity_repositories(backend);
              register_role_repositories(backend);
              register_audit_repositories(backend);
            },
            [](PostgresBackend& backend) {
              register_identity_repositories(backend);
              register_role_repositories(backend);
              register_audit_repositories(backend);
            });
      }

      [[nodiscard]] IStorageBackend& backend() {
        return harness_->backend();
      }

    private:
      std::unique_ptr<RepoBackendHarness> harness_;
    };

    TEST_P(RoleRepositoryTest, MigrationSeedsBuiltinRolesAndPermissionGrants) {
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

    TEST_P(RoleRepositoryTest, CustomRoleInsertAndArchiveRoundTrip) {
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

    TEST_P(RoleRepositoryTest, ArchivingBuiltinRoleIsRejected) {
      auto transaction = backend().begin(IsolationLevel::Serializable);
      EXPECT_THROW(transaction->repo<core::Role>().soft_delete(
                       core::builtin_role_id(core::RoleKind::SystemAdmin), mutation_context()),
                   ConstraintViolation);
    }

    TEST_P(RoleRepositoryTest, GrantingPermissionToMissingRoleIsForeignKeyViolation) {
      auto transaction = backend().begin(IsolationLevel::Serializable);
      transaction->repo<core::RolePermission>().insert(
          core::RolePermission{
              .role_id = id_from_low<core::RoleId>(9999),
              .permission = core::Permission::SampleRead,
          },
          mutation_context());

      EXPECT_THROW(transaction->commit(), ForeignKeyViolation);
    }

    TEST_P(RoleRepositoryTest, RevokingPermissionDeletesRow) {
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

    TEST_P(RoleRepositoryTest, LabMembershipPersistsRoleId) {
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

    TEST_P(RoleRepositoryTest, LabMembershipWithMissingRoleIsForeignKeyViolation) {
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

    TEST_P(RoleRepositoryTest, DuplicateRoleNameInSameLabThrowsUniqueViolation) {
      const core::Lab lab{.id = id_from_low<core::LabId>(2001),
                          .name = "DupRoleLab",
                          .contact = "lab@example.org",
                          .created_at = core::Timestamp::from_unix_micros(1),
                          .settings_json = nlohmann::json::object()};
      auto seed = backend().begin(IsolationLevel::Serializable);
      seed->repo<core::Lab>().insert(lab, mutation_context());
      seed->commit();

      auto txn = backend().begin(IsolationLevel::Serializable);
      txn->repo<core::Role>().insert(
          core::Role{.id = id_from_low<core::RoleId>(2002),
                     .lab_id = lab.id,
                     .kind = core::RoleKind::Member,
                     .name = "Conflicting",
                     .description = "First",
                     .is_builtin = false,
                     .created_at = core::Timestamp::from_unix_micros(10)},
          mutation_context());
      // The second same-name role collides on the (lab_id, name) unique index.
      // SQLite stages both and fails the index check at commit; Postgres writes
      // immediately and fails on the second insert. Wrap both so either path is
      // caught.
      EXPECT_THROW(
          {
            txn->repo<core::Role>().insert(
                core::Role{.id = id_from_low<core::RoleId>(2003),
                           .lab_id = lab.id,
                           .kind = core::RoleKind::Member,
                           .name = "Conflicting",
                           .description = "Second",
                           .is_builtin = false,
                           .created_at = core::Timestamp::from_unix_micros(11)},
                mutation_context());
            txn->commit();
          },
          UniqueViolation);
    }

    TEST_P(RoleRepositoryTest, DuplicatePermissionGrantThrowsUniqueViolation) {
      const core::Lab lab{.id = id_from_low<core::LabId>(2101),
                          .name = "GrantDupLab",
                          .contact = "lab@example.org",
                          .created_at = core::Timestamp::from_unix_micros(1),
                          .settings_json = nlohmann::json::object()};
      auto seed = backend().begin(IsolationLevel::Serializable);
      seed->repo<core::Lab>().insert(lab, mutation_context());
      seed->commit();

      const auto role_id = id_from_low<core::RoleId>(2102);
      auto txn = backend().begin(IsolationLevel::Serializable);
      txn->repo<core::Role>().insert(
          core::Role{.id = role_id,
                     .lab_id = lab.id,
                     .kind = core::RoleKind::Member,
                     .name = "Duplicate Grant Role",
                     .description = "Test duplicate grants",
                     .is_builtin = false,
                     .created_at = core::Timestamp::from_unix_micros(10)},
          mutation_context());
      txn->repo<core::RolePermission>().insert(
          core::RolePermission{.role_id = role_id, .permission = core::Permission::AuditExport},
          mutation_context());
      // Second insert with same (role_id, permission) — both backends reject it
      // at insert time (SQLite via stage-time check, Postgres via the PK).
      EXPECT_THROW(
          txn->repo<core::RolePermission>().insert(
              core::RolePermission{.role_id = role_id, .permission = core::Permission::AuditExport},
              mutation_context()),
          UniqueViolation);
    }

    TEST_P(RoleRepositoryTest, RoleUpdateRecordsBeforeAndAfterAuditSnapshot) {
      const core::Lab lab{
          .id = id_from_low<core::LabId>(3001),
          .name = "Audit Lab",
          .contact = "lab@example.org",
          .created_at = core::Timestamp::from_unix_micros(1),
          .settings_json = nlohmann::json::object(),
      };
      auto seed = backend().begin(IsolationLevel::Serializable);
      seed->repo<core::Lab>().insert(lab, mutation_context());
      seed->commit();

      core::Role role{
          .id = id_from_low<core::RoleId>(3002),
          .lab_id = lab.id,
          .kind = core::RoleKind::Member,
          .name = "Original Name",
          .description = "before-image test role",
          .is_builtin = false,
          .created_at = core::Timestamp::from_unix_micros(10),
      };
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Role>().insert(role, mutation_context());
        txn->commit();
      }

      role.name = "Renamed Role";
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Role>().update(role, mutation_context());
        txn->commit();
      }

      auto txn = backend().begin(IsolationLevel::Serializable);
      const auto events = txn->repo<core::AuditEvent>().query(Query<core::AuditEvent>::where(
          field<core::AuditEvent, std::string>(core::AuditEvent::Field::EntityId) ==
          role.id.to_string()));
      txn->commit();
      ASSERT_EQ(events.size(), 2U);

      const auto update_it =
          std::find_if(events.begin(), events.end(),
                       [](const core::AuditEvent& event) { return event.action == "update"; });
      ASSERT_NE(update_it, events.end());
      // SQLite uses a generic staging path that records only after on update;
      // Postgres now captures the committed row before the write.
      if (GetParam() == BackendKind::Postgres) {
        EXPECT_NE(update_it->before_json.find("Original Name"), std::string::npos);
      }
      EXPECT_NE(update_it->after_json.find("Renamed Role"), std::string::npos);
    }

    INSTANTIATE_TEST_SUITE_P(Backends, RoleRepositoryTest,
                             ::testing::Values(BackendKind::Sqlite, BackendKind::Postgres),
                             [](const ::testing::TestParamInfo<BackendKind>& info) {
                               return backend_kind_name(info.param);
                             });

  } // namespace
} // namespace fmgr::storage
