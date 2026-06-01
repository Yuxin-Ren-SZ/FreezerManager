// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/identity.h"
#include "core/session.h"
#include "storage/IdentityTraits.h"
#include "storage/SessionTraits.h"
#include "storage/sqlite/IdentityRepositories.h"
#include "storage/sqlite/SessionRepositories.h"
#include "storage/sqlite/SqliteBackend.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <filesystem>
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

    [[nodiscard]] core::Timestamp ts(std::int64_t micros) {
      return core::Timestamp::from_unix_micros(micros);
    }

    [[nodiscard]] std::filesystem::path sqlite_test_path(std::string_view suffix) {
      const auto unique = std::to_string(static_cast<unsigned long long>(
                              ::testing::UnitTest::GetInstance()->random_seed())) +
                          "-" + std::to_string(reinterpret_cast<std::uintptr_t>(&suffix));
      return std::filesystem::temp_directory_path() / (std::string("freezermanager-sqlite-sess-") +
                                                       unique + "-" + std::string(suffix) + ".db");
    }

    void remove_sqlite_files(const std::filesystem::path& path) {
      std::filesystem::remove(path);
      std::filesystem::remove(path.string() + "-wal");
      std::filesystem::remove(path.string() + "-shm");
    }

    [[nodiscard]] MutationContext mutation_context() {
      return MutationContext{
          .actor_user_id = id_from_low<core::UserId>(999),
          .actor_session_id = "sess-test-session",
          .request_id = "sess-test-request",
          .reason = "session repository test",
      };
    }

    [[nodiscard]] core::Lab make_lab(std::uint64_t low_bits) {
      return core::Lab{
          .id = id_from_low<core::LabId>(low_bits),
          .name = "Lab " + std::to_string(low_bits),
          .contact = "lab@example.org",
          .created_at = ts(100 + static_cast<std::int64_t>(low_bits)),
          .settings_json = nlohmann::json::object(),
      };
    }

    [[nodiscard]] core::User make_user(std::uint64_t low_bits, core::LabId lab_id) {
      return core::User{
          .id = id_from_low<core::UserId>(low_bits),
          .primary_email = "user" + std::to_string(low_bits) + "@example.org",
          .display_name = "Test User",
          .status = core::UserStatus::Active,
          .created_at = ts(200 + static_cast<std::int64_t>(low_bits)),
          .auth_bindings = nlohmann::json::array({{{"provider", "local"}}}),
          .default_lab_id = lab_id,
      };
    }

    [[nodiscard]] core::Session make_session(std::uint64_t low_bits, core::UserId user_id) {
      return core::Session{
          .id = id_from_low<core::SessionId>(low_bits),
          .user_id = user_id,
          .token_hash = "argon2id$hash" + std::to_string(low_bits),
          .token_prefix = "prefix" + std::to_string(low_bits),
          .created_at = ts(1000 + static_cast<std::int64_t>(low_bits)),
          .last_seen_at = ts(2000 + static_cast<std::int64_t>(low_bits)),
          .ip = "127.0.0.1",
          .user_agent = "test-agent/1.0",
      };
    }

    [[nodiscard]] core::ApiToken make_api_token(std::uint64_t low_bits, core::UserId user_id,
                                                std::optional<core::LabId> lab_id) {
      return core::ApiToken{
          .id = id_from_low<core::ApiTokenId>(low_bits),
          .user_id = user_id,
          .lab_id = lab_id,
          .name = "Token " + std::to_string(low_bits),
          .scope_json = R"(["sample.read"])",
          .token_hash = "api_hash" + std::to_string(low_bits),
          .token_prefix = "apipre" + std::to_string(low_bits),
          .created_at = ts(3000 + static_cast<std::int64_t>(low_bits)),
          .expires_at = ts(4000 + static_cast<std::int64_t>(low_bits)),
      };
    }

    // ---- Test fixture ----

    class SqliteSessionRepositoryTest : public ::testing::Test {
    protected:
      SqliteSessionRepositoryTest()
          : db_path_(sqlite_test_path("sess-repo")),
            backend_(SqliteBackendOptions{.database_path = db_path_.string()}) {
        register_identity_repositories(backend_);
        register_session_repositories(backend_);
      }

      void SetUp() override {
        remove_sqlite_files(db_path_);
        backend_.migrate_to_latest();

        {
          auto txn = backend().begin(IsolationLevel::Serializable);
          txn->repo<core::Lab>().insert(make_lab(1), mutation_context());
          txn->commit();
        }
        {
          auto txn = backend().begin(IsolationLevel::Serializable);
          txn->repo<core::User>().insert(make_user(10, id_from_low<core::LabId>(1)),
                                         mutation_context());
          txn->commit();
        }
        {
          auto txn = backend().begin(IsolationLevel::Serializable);
          txn->repo<core::User>().insert(make_user(20, id_from_low<core::LabId>(1)),
                                         mutation_context());
          txn->commit();
        }

        lab1_ = id_from_low<core::LabId>(1);
        user1_ = id_from_low<core::UserId>(10);
        user2_ = id_from_low<core::UserId>(20);
      }

      void TearDown() override {
        remove_sqlite_files(db_path_);
      }

      [[nodiscard]] SqliteBackend& backend() {
        return backend_;
      }

      std::filesystem::path db_path_;
      SqliteBackend backend_;
      core::LabId lab1_;
      core::UserId user1_;
      core::UserId user2_;
    };

    // ---- Session tests ----

    TEST_F(SqliteSessionRepositoryTest, InsertAndFindById) {
      const auto session = make_session(1, user1_);
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Session>().insert(session, mutation_context());
        txn->commit();
      }
      auto txn = backend().begin(IsolationLevel::Serializable);
      const auto found = txn->repo<core::Session>().find_by_id(session.id);
      ASSERT_TRUE(found.has_value());
      EXPECT_EQ(found->id, session.id);
      EXPECT_EQ(found->user_id, user1_);
      EXPECT_EQ(found->token_hash, session.token_hash);
      EXPECT_EQ(found->token_prefix, session.token_prefix);
      EXPECT_FALSE(found->revoked_at.has_value());
    }

    TEST_F(SqliteSessionRepositoryTest, DuplicateIdThrowsUniqueViolation) {
      const auto session = make_session(1, user1_);
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Session>().insert(session, mutation_context());
        txn->commit();
      }
      auto txn = backend().begin(IsolationLevel::Serializable);
      EXPECT_THROW(txn->repo<core::Session>().insert(session, mutation_context()), UniqueViolation);
    }

    TEST_F(SqliteSessionRepositoryTest, DuplicateActivePrefixThrowsUniqueViolationAtCommit) {
      const auto session1 = make_session(1, user1_);
      core::Session session2 = make_session(2, user2_);
      // Force same prefix as session1 to trigger the partial unique index at flush.
      session2.token_prefix = session1.token_prefix;
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Session>().insert(session1, mutation_context());
        txn->commit();
      }
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Session>().insert(session2, mutation_context());
        // Prefix uniqueness is enforced by the DB partial index at flush time (commit).
        EXPECT_THROW(txn->commit(), UniqueViolation);
      }
    }

    TEST_F(SqliteSessionRepositoryTest, RevokedSessionAllowsSamePrefixForNewSession) {
      auto session1 = make_session(1, user1_);
      core::Session session2 = make_session(2, user2_);
      session2.token_prefix = session1.token_prefix;
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Session>().insert(session1, mutation_context());
        txn->commit();
      }
      // Revoke session1.
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Session>().soft_delete(session1.id, mutation_context());
        txn->commit();
      }
      // Now session2 with the same prefix can be inserted.
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Session>().insert(session2, mutation_context());
        txn->commit();
      }
      auto txn = backend().begin(IsolationLevel::Serializable);
      const auto found = txn->repo<core::Session>().find_by_id(session2.id);
      ASSERT_TRUE(found.has_value());
      EXPECT_FALSE(found->revoked_at.has_value());
    }

    TEST_F(SqliteSessionRepositoryTest, DefaultQueryExcludesRevokedSessions) {
      const auto session = make_session(1, user1_);
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Session>().insert(session, mutation_context());
        txn->commit();
      }
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Session>().soft_delete(session.id, mutation_context());
        txn->commit();
      }
      // Default query: active sessions only.
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        const auto results = txn->repo<core::Session>().query(Query<core::Session>{});
        EXPECT_TRUE(results.empty());
      }
      // include_tombstoned shows revoked records.
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        const auto results =
            txn->repo<core::Session>().query(Query<core::Session>{}.include_tombstoned());
        ASSERT_EQ(results.size(), 1u);
        EXPECT_EQ(results.front().id, session.id);
        EXPECT_TRUE(results.front().revoked_at.has_value());
      }
    }

    TEST_F(SqliteSessionRepositoryTest, SoftDeleteSetsRevokedAt) {
      const auto session = make_session(1, user1_);
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Session>().insert(session, mutation_context());
        txn->commit();
      }
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Session>().soft_delete(session.id, mutation_context());
        txn->commit();
      }
      auto txn = backend().begin(IsolationLevel::Serializable);
      const auto found = txn->repo<core::Session>().find_by_id(session.id);
      ASSERT_TRUE(found.has_value());
      EXPECT_TRUE(found->revoked_at.has_value());
    }

    TEST_F(SqliteSessionRepositoryTest, SoftDeleteNonExistentThrowsNotFound) {
      auto txn = backend().begin(IsolationLevel::Serializable);
      const auto fake_id = id_from_low<core::SessionId>(999);
      EXPECT_THROW(txn->repo<core::Session>().soft_delete(fake_id, mutation_context()), NotFound);
    }

    TEST_F(SqliteSessionRepositoryTest, UpdateLastSeenAt) {
      const auto session = make_session(1, user1_);
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Session>().insert(session, mutation_context());
        txn->commit();
      }
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        auto updated_session = session;
        updated_session.last_seen_at = ts(99999);
        txn->repo<core::Session>().update(updated_session, mutation_context());
        txn->commit();
      }
      auto txn = backend().begin(IsolationLevel::Serializable);
      const auto found = txn->repo<core::Session>().find_by_id(session.id);
      ASSERT_TRUE(found.has_value());
      EXPECT_EQ(found->last_seen_at.unix_micros(), 99999);
    }

    TEST_F(SqliteSessionRepositoryTest, QueryByUserId) {
      const auto s1 = make_session(1, user1_);
      const auto s2 = make_session(2, user2_);
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Session>().insert(s1, mutation_context());
        txn->repo<core::Session>().insert(s2, mutation_context());
        txn->commit();
      }
      auto txn = backend().begin(IsolationLevel::Serializable);
      const auto results = txn->repo<core::Session>().query(Query<core::Session>::where(
          field<core::Session, std::string>(core::Session::Field::UserId) == user1_.to_string()));
      ASSERT_EQ(results.size(), 1u);
      EXPECT_EQ(results.front().id, s1.id);
    }

    TEST_F(SqliteSessionRepositoryTest, QueryByTokenPrefix) {
      const auto session = make_session(1, user1_);
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Session>().insert(session, mutation_context());
        txn->commit();
      }
      auto txn = backend().begin(IsolationLevel::Serializable);
      const auto results = txn->repo<core::Session>().query(Query<core::Session>::where(
          field<core::Session, std::string>(core::Session::Field::TokenPrefix) ==
          session.token_prefix));
      ASSERT_EQ(results.size(), 1u);
      EXPECT_EQ(results.front().id, session.id);
    }

    TEST_F(SqliteSessionRepositoryTest, EmptyTokenHashThrowsConstraintViolation) {
      auto session = make_session(1, user1_);
      session.token_hash = "";
      auto txn = backend().begin(IsolationLevel::Serializable);
      EXPECT_THROW(txn->repo<core::Session>().insert(session, mutation_context()),
                   ConstraintViolation);
    }

    TEST_F(SqliteSessionRepositoryTest, EmptyTokenPrefixThrowsConstraintViolation) {
      auto session = make_session(1, user1_);
      session.token_prefix = "";
      auto txn = backend().begin(IsolationLevel::Serializable);
      EXPECT_THROW(txn->repo<core::Session>().insert(session, mutation_context()),
                   ConstraintViolation);
    }

    // ---- ApiToken tests ----

    TEST_F(SqliteSessionRepositoryTest, ApiTokenInsertAndFindById) {
      const auto token = make_api_token(1, user1_, lab1_);
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::ApiToken>().insert(token, mutation_context());
        txn->commit();
      }
      auto txn = backend().begin(IsolationLevel::Serializable);
      const auto found = txn->repo<core::ApiToken>().find_by_id(token.id);
      ASSERT_TRUE(found.has_value());
      EXPECT_EQ(found->id, token.id);
      EXPECT_EQ(found->user_id, user1_);
      ASSERT_TRUE(found->lab_id.has_value());
      EXPECT_EQ(*found->lab_id, lab1_);
      EXPECT_EQ(found->name, token.name);
      EXPECT_FALSE(found->revoked_at.has_value());
    }

    TEST_F(SqliteSessionRepositoryTest, ApiTokenWithoutLabId) {
      const auto token = make_api_token(2, user1_, std::nullopt);
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::ApiToken>().insert(token, mutation_context());
        txn->commit();
      }
      auto txn = backend().begin(IsolationLevel::Serializable);
      const auto found = txn->repo<core::ApiToken>().find_by_id(token.id);
      ASSERT_TRUE(found.has_value());
      EXPECT_FALSE(found->lab_id.has_value());
    }

    TEST_F(SqliteSessionRepositoryTest, ApiTokenSoftDeleteSetsRevokedAt) {
      const auto token = make_api_token(1, user1_, lab1_);
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::ApiToken>().insert(token, mutation_context());
        txn->commit();
      }
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::ApiToken>().soft_delete(token.id, mutation_context());
        txn->commit();
      }
      auto txn = backend().begin(IsolationLevel::Serializable);
      const auto found = txn->repo<core::ApiToken>().find_by_id(token.id);
      ASSERT_TRUE(found.has_value());
      EXPECT_TRUE(found->revoked_at.has_value());
    }

    TEST_F(SqliteSessionRepositoryTest, ApiTokenDefaultQueryExcludesRevoked) {
      const auto token = make_api_token(1, user1_, lab1_);
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::ApiToken>().insert(token, mutation_context());
        txn->commit();
      }
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::ApiToken>().soft_delete(token.id, mutation_context());
        txn->commit();
      }
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        const auto results = txn->repo<core::ApiToken>().query(Query<core::ApiToken>{});
        EXPECT_TRUE(results.empty());
      }
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        const auto results =
            txn->repo<core::ApiToken>().query(Query<core::ApiToken>{}.include_tombstoned());
        ASSERT_EQ(results.size(), 1u);
        EXPECT_EQ(results.front().id, token.id);
        EXPECT_TRUE(results.front().revoked_at.has_value());
      }
    }

    TEST_F(SqliteSessionRepositoryTest,
           ApiTokenDuplicateActivePrefixThrowsUniqueViolationAtCommit) {
      const auto token1 = make_api_token(1, user1_, lab1_);
      core::ApiToken token2 = make_api_token(2, user2_, std::nullopt);
      token2.token_prefix = token1.token_prefix; // collision
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::ApiToken>().insert(token1, mutation_context());
        txn->commit();
      }
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::ApiToken>().insert(token2, mutation_context());
        // Prefix uniqueness enforced by partial unique index at flush.
        EXPECT_THROW(txn->commit(), UniqueViolation);
      }
    }

    TEST_F(SqliteSessionRepositoryTest, ApiTokenEmptyNameThrowsConstraintViolation) {
      auto token = make_api_token(1, user1_, std::nullopt);
      token.name = "";
      auto txn = backend().begin(IsolationLevel::Serializable);
      EXPECT_THROW(txn->repo<core::ApiToken>().insert(token, mutation_context()),
                   ConstraintViolation);
    }

    TEST_F(SqliteSessionRepositoryTest, ApiTokenQueryByUserId) {
      const auto t1 = make_api_token(1, user1_, lab1_);
      const auto t2 = make_api_token(2, user2_, std::nullopt);
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::ApiToken>().insert(t1, mutation_context());
        txn->repo<core::ApiToken>().insert(t2, mutation_context());
        txn->commit();
      }
      auto txn = backend().begin(IsolationLevel::Serializable);
      const auto results = txn->repo<core::ApiToken>().query(Query<core::ApiToken>::where(
          field<core::ApiToken, std::string>(core::ApiToken::Field::UserId) == user1_.to_string()));
      ASSERT_EQ(results.size(), 1u);
      EXPECT_EQ(results.front().id, t1.id);
    }

    TEST_F(SqliteSessionRepositoryTest, ApiTokenMutationsAreAudited) {
      const auto token = make_api_token(1, user1_, lab1_);
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::ApiToken>().insert(token, mutation_context());
        txn->commit();
      }
      // At least 1 audit row should have been inserted.
      EXPECT_GE(backend().audit_event_count_for_tests(), 1u);
    }

  } // namespace
} // namespace fmgr::storage
