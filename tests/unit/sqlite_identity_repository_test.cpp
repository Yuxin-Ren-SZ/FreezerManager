// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/identity.h"
#include "storage/IdentityTraits.h"
#include "storage/sqlite/IdentityRepositories.h"
#include "storage/sqlite/SqliteBackend.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
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
      return std::filesystem::temp_directory_path() /
             (std::string("freezermanager-sqlite-identity-") + unique + "-" + std::string(suffix) +
              ".db");
    }

    void remove_sqlite_files(const std::filesystem::path& path) {
      std::filesystem::remove(path);
      std::filesystem::remove(path.string() + "-wal");
      std::filesystem::remove(path.string() + "-shm");
    }

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

    class SqliteIdentityRepositoryTest : public ::testing::Test {
    protected:
      SqliteIdentityRepositoryTest()
          : db_path_(sqlite_test_path("repositories")), backend_(SqliteBackendOptions{
                                                            .database_path = db_path_.string(),
                                                        }) {
        register_identity_repositories(backend_);
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

    TEST_F(SqliteIdentityRepositoryTest, LabCrudQueryAndSoftDeleteRoundTrip) {
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

    TEST_F(SqliteIdentityRepositoryTest, UserEmailUniquenessIsCaseInsensitive) {
      auto transaction = backend().begin(IsolationLevel::Serializable);
      auto& repository = transaction->repo<core::User>();
      repository.insert(user(10, "Ada@Example.ORG"), mutation_context());
      repository.insert(user(11, "ada@example.org"), mutation_context());

      EXPECT_THROW(transaction->commit(), UniqueViolation);
    }

    TEST_F(SqliteIdentityRepositoryTest, UserQueryFiltersDisabledRowsByDefault) {
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

    TEST_F(SqliteIdentityRepositoryTest, MembershipCrudAndRevocationUseCompositeId) {
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

    TEST_F(SqliteIdentityRepositoryTest, MembershipRejectsMissingUserOrLab) {
      auto transaction = backend().begin(IsolationLevel::Serializable);
      transaction->repo<core::LabMembership>().insert(
          membership(id_from_low<core::UserId>(404), id_from_low<core::LabId>(405)),
          mutation_context());

      EXPECT_THROW(transaction->commit(), ForeignKeyViolation);
    }

    TEST_F(SqliteIdentityRepositoryTest, MutationsAppendAuditEvents) {
      auto transaction = backend().begin(IsolationLevel::Serializable);
      transaction->repo<core::Lab>().insert(lab(50, "Audited Lab"), mutation_context());
      transaction->commit();

      EXPECT_EQ(backend().audit_event_count_for_tests(), 1U);
    }

    TEST_F(SqliteIdentityRepositoryTest, UpdateNonexistentUserThrowsNotFound) {
      auto transaction = backend().begin(IsolationLevel::Serializable);
      auto entity = user(99, "nonexistent@example.org");
      EXPECT_THROW(transaction->repo<core::User>().update(entity, mutation_context()), NotFound);
    }

    TEST_F(SqliteIdentityRepositoryTest, UpdateNonexistentLabMembershipThrowsNotFound) {
      auto transaction = backend().begin(IsolationLevel::Serializable);
      auto entity = membership(id_from_low<core::UserId>(9999), id_from_low<core::LabId>(9999));
      EXPECT_THROW(transaction->repo<core::LabMembership>().update(entity, mutation_context()),
                   NotFound);
    }

  } // namespace
} // namespace fmgr::storage
