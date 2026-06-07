// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/identity.h"
#include "storage/IdentityTraits.h"
#include "storage/postgres/IdentityRepositories.h"
#include "storage/sqlite/IdentityRepositories.h"

#include "repo_backend_harness.h"
#include "test_helpers.h"
#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace fmgr::storage {
  namespace {
    using namespace fmgr::test;

    [[nodiscard]] MutationContext mutation_context() {
      return MutationContext{
          .actor_user_id = id_from_low<core::UserId>(900),
          .actor_session_id = "identity-session",
          .request_id = "identity-request",
          .reason = "identity repository test",
      };
    }

    [[nodiscard]] core::Lab lab(std::uint64_t id_low_bits, std::string name) {
      return core::Lab{
          .id = id_from_low<core::LabId>(id_low_bits),
          .name = std::move(name),
          .contact = "lab-admin@example.org",
          .created_at =
              core::Timestamp::from_unix_micros(100 + static_cast<std::int64_t>(id_low_bits)),
          .settings_json = nlohmann::json{{"timezone", "UTC"}},
          .is_phi_enabled = false,
      };
    }

    [[nodiscard]] core::User user(std::uint64_t id_low_bits, std::string email,
                                  std::optional<core::LabId> default_lab_id = std::nullopt) {
      return core::User{
          .id = id_from_low<core::UserId>(id_low_bits),
          .primary_email = std::move(email),
          .display_name = "Identity User",
          .status = core::UserStatus::Active,
          .created_at =
              core::Timestamp::from_unix_micros(200 + static_cast<std::int64_t>(id_low_bits)),
          .auth_bindings = nlohmann::json::array({{{"provider", "local"}}}),
          .default_lab_id = default_lab_id,
      };
    }

    [[nodiscard]] core::LabMembership membership(core::UserId user_id, core::LabId lab_id) {
      return core::LabMembership{
          .user_id = user_id,
          .lab_id = lab_id,
          .scope_filters_json = nlohmann::json::object(),
          .invited_by = user_id,
          .joined_at = core::Timestamp::from_unix_micros(300),
      };
    }

    class IdentityRepositoryTest : public ::testing::TestWithParam<BackendKind> {
    protected:
      void SetUp() override {
        if (GetParam() == BackendKind::Postgres && !postgres_test_url().has_value()) {
          GTEST_SKIP() << "FMGR_TEST_POSTGRES_URL not set; skipping Postgres repository tests";
        }
        harness_ = std::make_unique<RepoBackendHarness>(
            GetParam(), [](SqliteBackend& backend) { register_identity_repositories(backend); },
            [](PostgresBackend& backend) { register_identity_repositories(backend); });
      }

      [[nodiscard]] IStorageBackend& backend() {
        return harness_->backend();
      }

      [[nodiscard]] std::size_t audit_event_count() {
        return harness_->audit_event_count();
      }

    private:
      std::unique_ptr<RepoBackendHarness> harness_;
    };

    TEST_P(IdentityRepositoryTest, LabCrudQueryAndSoftDeleteRoundTrip) {
      auto entity = lab(1, "Neuro Bank");
      auto transaction = backend().begin(IsolationLevel::Serializable);
      auto& repository = transaction->repo<core::Lab>();
      repository.insert(entity, mutation_context());
      transaction->commit();

      transaction = backend().begin(IsolationLevel::Serializable);
      auto& read_repository = transaction->repo<core::Lab>();
      auto stored = read_repository.find_by_id(entity.id);
      ASSERT_TRUE(stored.has_value());
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
      EXPECT_EQ(stored.value().name, "Neuro Bank");

      entity.name = "Neuro Repository";
      read_repository.update(entity, mutation_context());
      read_repository.soft_delete(entity.id, mutation_context());
      transaction->commit();

      transaction = backend().begin(IsolationLevel::Serializable);
      auto& query_repository = transaction->repo<core::Lab>();
      EXPECT_TRUE(query_repository.query(Query<core::Lab>::all()).empty());

      const auto archived = query_repository.query(Query<core::Lab>::all().include_tombstoned());
      ASSERT_EQ(archived.size(), 1U);
      EXPECT_EQ(archived.front().name, "Neuro Repository");
      EXPECT_TRUE(archived.front().archived_at.has_value());
    }

    TEST_P(IdentityRepositoryTest, UserEmailUniquenessIsCaseInsensitive) {
      auto transaction = backend().begin(IsolationLevel::Serializable);
      auto& repository = transaction->repo<core::User>();
      repository.insert(user(10, "Ada@Example.ORG"), mutation_context());
      EXPECT_THROW(
          {
            repository.insert(user(11, "ada@example.org"), mutation_context());
            transaction->commit();
          },
          UniqueViolation);
    }

    TEST_P(IdentityRepositoryTest, UserQueryFiltersDisabledRowsByDefault) {
      auto entity = user(20, "enabled@example.org");
      auto transaction = backend().begin(IsolationLevel::Serializable);
      auto& repository = transaction->repo<core::User>();
      repository.insert(entity, mutation_context());
      repository.soft_delete(entity.id, mutation_context());
      transaction->commit();

      transaction = backend().begin(IsolationLevel::Serializable);
      auto& query_repository = transaction->repo<core::User>();
      EXPECT_TRUE(query_repository.query(Query<core::User>::all()).empty());

      const auto disabled = query_repository.query(Query<core::User>::all().include_tombstoned());
      ASSERT_EQ(disabled.size(), 1U);
      EXPECT_EQ(disabled.front().status, core::UserStatus::Disabled);
    }

    TEST_P(IdentityRepositoryTest, MembershipCrudAndRevocationUseCompositeId) {
      const auto lab_entity = lab(30, "Immunology");
      const auto user_entity = user(31, "member@example.org", lab_entity.id);
      const auto membership_entity = membership(user_entity.id, lab_entity.id);

      auto transaction = backend().begin(IsolationLevel::Serializable);
      transaction->repo<core::Lab>().insert(lab_entity, mutation_context());
      transaction->repo<core::User>().insert(user_entity, mutation_context());
      auto& membership_repository = transaction->repo<core::LabMembership>();
      membership_repository.insert(membership_entity, mutation_context());
      transaction->commit();

      transaction = backend().begin(IsolationLevel::Serializable);
      auto& read_repository = transaction->repo<core::LabMembership>();
      const auto membership_id = membership_entity.id();
      ASSERT_TRUE(read_repository.find_by_id(membership_id).has_value());
      read_repository.soft_delete(membership_id, mutation_context());
      transaction->commit();

      transaction = backend().begin(IsolationLevel::Serializable);
      auto& query_repository = transaction->repo<core::LabMembership>();
      EXPECT_TRUE(query_repository.query(Query<core::LabMembership>::all()).empty());
      const auto revoked =
          query_repository.query(Query<core::LabMembership>::all().include_tombstoned());
      ASSERT_EQ(revoked.size(), 1U);
      EXPECT_TRUE(revoked.front().revoked_at.has_value());
    }

    TEST_P(IdentityRepositoryTest, MembershipRejectsMissingUserOrLab) {
      auto transaction = backend().begin(IsolationLevel::Serializable);
      EXPECT_THROW(
          {
            transaction->repo<core::LabMembership>().insert(
                membership(id_from_low<core::UserId>(404), id_from_low<core::LabId>(405)),
                mutation_context());
            transaction->commit();
          },
          ForeignKeyViolation);
    }

    TEST_P(IdentityRepositoryTest, MutationsAppendAuditEvents) {
      auto transaction = backend().begin(IsolationLevel::Serializable);
      transaction->repo<core::Lab>().insert(lab(50, "Audited Lab"), mutation_context());
      transaction->commit();

      EXPECT_EQ(audit_event_count(), 1U);
    }

    TEST_P(IdentityRepositoryTest, UpdateNonexistentUserThrowsNotFound) {
      auto transaction = backend().begin(IsolationLevel::Serializable);
      auto entity = user(99, "nonexistent@example.org");
      EXPECT_THROW(transaction->repo<core::User>().update(entity, mutation_context()), NotFound);
    }

    TEST_P(IdentityRepositoryTest, UpdateNonexistentLabMembershipThrowsNotFound) {
      auto transaction = backend().begin(IsolationLevel::Serializable);
      auto entity = membership(id_from_low<core::UserId>(9999), id_from_low<core::LabId>(9999));
      EXPECT_THROW(transaction->repo<core::LabMembership>().update(entity, mutation_context()),
                   NotFound);
    }

    INSTANTIATE_TEST_SUITE_P(Backends, IdentityRepositoryTest,
                             ::testing::Values(BackendKind::Sqlite, BackendKind::Postgres),
                             [](const ::testing::TestParamInfo<BackendKind>& info) {
                               return backend_kind_name(info.param);
                             });

  } // namespace
} // namespace fmgr::storage
