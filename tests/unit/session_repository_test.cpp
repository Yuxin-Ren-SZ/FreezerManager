// SPDX-License-Identifier: AGPL-3.0-or-later

#include "audit/AuditChainVerifier.h"
#include "auth/LocalAuthProvider.h"
#include "auth/Totp.h"
#include "core/identity.h"
#include "core/session.h"
#include "storage/AuditTraits.h"
#include "storage/IdentityTraits.h"
#include "storage/MfaSecurityStateTraits.h"
#include "storage/SessionTraits.h"
#include "storage/postgres/AuditRepositories.h"
#include "storage/postgres/IdentityRepositories.h"
#include "storage/postgres/PostgresRepoSupport.h"
#include "storage/postgres/RoleRepositories.h"
#include "storage/postgres/SessionRepositories.h"
#include "storage/sqlite/AuditRepositories.h"
#include "storage/sqlite/IdentityRepositories.h"
#include "storage/sqlite/RoleRepositories.h"
#include "storage/sqlite/SessionRepositories.h"

#include "paused_activity_backend.h"
#include "repo_backend_harness.h"
#include "test_helpers.h"
#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace fmgr::storage {
  namespace {
    using namespace fmgr::test;

    TEST(SessionAuthErrorMapping, PostgreSqlRepositoryLockTimeoutIsUnavailable) {
      const pqxx::sql_error error("injected lock timeout", "SELECT 1", "55P03");
      EXPECT_THROW(detail::throw_pqxx_error(error), Unavailable);
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

    // Privileged fixture SQL only for testing engine constraints and legacy migrations.
    void mfa_fixture_sql(ITransaction& txn, const std::string& sql) {
      if (auto* sqlite = dynamic_cast<SqliteTransaction*>(&txn)) {
        if (sqlite3_exec(sqlite->handle(), sql.c_str(), nullptr, nullptr, nullptr) != SQLITE_OK) {
          throw ConstraintViolation("MFA fixture SQL refused");
        }
      } else {
        try {
          dynamic_cast<PostgresTransaction&>(txn).work().exec(sql);
        } catch (const pqxx::sql_error& error) {
          detail::throw_pqxx_error(error);
        }
      }
    }

    // ---- Test fixture ----

    class SessionRepositoryTest : public ::testing::TestWithParam<BackendKind> {
    protected:
      void SetUp() override {
        if (GetParam() == BackendKind::Postgres && !postgres_test_url().has_value()) {
          GTEST_SKIP() << "FMGR_TEST_POSTGRES_URL not set; skipping Postgres repository tests";
        }
        harness_ = std::make_unique<RepoBackendHarness>(
            GetParam(),
            [](SqliteBackend& b) {
              register_identity_repositories(b);
              register_audit_repositories(b);
              register_role_repositories(b);
              register_session_repositories(b);
            },
            [](PostgresBackend& b) {
              register_identity_repositories(b);
              register_audit_repositories(b);
              register_role_repositories(b);
              register_session_repositories(b);
            });

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

      [[nodiscard]] IStorageBackend& backend() {
        return harness_->backend();
      }

      [[nodiscard]] std::size_t audit_event_count() {
        return harness_->audit_event_count();
      }

      void fail_next_audit_append() {
        if (GetParam() == BackendKind::Sqlite) {
          dynamic_cast<SqliteBackend&>(backend()).fail_next_audit_append_for_tests();
        } else {
          dynamic_cast<PostgresBackend&>(backend()).fail_next_audit_append_for_tests();
        }
      }

      [[nodiscard]] std::vector<core::AuditEvent> audit_events() {
        auto txn = backend().begin(IsolationLevel::ReadCommitted);
        auto events = txn->repo<core::AuditEvent>().query(Query<core::AuditEvent>::all());
        txn->commit();
        return events;
      }

      [[nodiscard]] static auth::LocalAuthProviderConfig auth_config() {
        auth::LocalAuthProviderConfig config;
        config.pwhash_memlimit = 8192;
        config.pwhash_opslimit = 1;
        return config;
      }

      [[nodiscard]] auth::AuthToken login_for_activity_test(bool needs_totp) {
        auth::LocalAuthProvider provider(backend(), auth_config());
        auto write = backend().begin(IsolationLevel::Serializable);
        auto user = write->repo<core::User>().find_by_id(user1_).value();
        user.auth_bindings = nlohmann::json::array(
            {{{"provider", "local"}, {"hash", provider.hash_password("fixture-password")}}});
        write->repo<core::User>().update(user, mutation_context());
        write->commit();
        if (needs_totp) {
          auto enroll = backend().begin_auth_transaction({user1_});
          enroll->replace_totp_secret(user1_, std::string(k_totp_secret), mutation_context());
          enroll->commit();
        }
        const auto token = provider.authenticate(
            auth::PasswordCredentials{.email = user.primary_email, .password = "fixture-password"},
            auth::ClientInfo{});
        auto backdate = backend().begin_auth_transaction({user1_});
        auto session = backdate->repo<core::Session>().find_by_id(token.session_id).value();
        const auto now = now_micros();
        session.created_at = core::Timestamp::from_unix_micros(now - 180'000'000);
        session.last_seen_at = core::Timestamp::from_unix_micros(now - 120'000'000);
        backdate->repo<core::Session>().update(session, mutation_context());
        backdate->commit();
        return token;
      }

      [[nodiscard]] static std::int64_t now_micros() {
        return std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
      }

      static void complete_mfa(auth::LocalAuthProvider& provider,
                               const core::SessionId& session_id) {
        provider.verify_totp(session_id,
                             auth::totp_generate(k_totp_secret, now_micros() / 1'000'000));
      }

      [[nodiscard]] core::Session read_session(const core::SessionId& session_id) {
        auto txn = backend().begin(IsolationLevel::ReadCommitted);
        auto session = txn->repo<core::Session>().find_by_id(session_id).value();
        txn->commit();
        return session;
      }

      static constexpr std::string_view k_totp_secret = "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ";

      std::unique_ptr<RepoBackendHarness> harness_;
      core::LabId lab1_;
      core::UserId user1_;
      core::UserId user2_;
    };

    // ---- Session tests ----

    TEST_P(SessionRepositoryTest, InsertAndFindById) {
      const auto session = make_session(1, user1_);
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Session>().insert(session, mutation_context());
        txn->commit();
      }
      auto txn = backend().begin(IsolationLevel::Serializable);
      const auto found = txn->repo<core::Session>().find_by_id(session.id);
      ASSERT_TRUE(found.has_value());
      // NOLINTBEGIN(bugprone-unchecked-optional-access): guarded by ASSERT_TRUE above
      EXPECT_EQ(found->id, session.id);
      EXPECT_EQ(found->user_id, user1_);
      EXPECT_EQ(found->token_hash, session.token_hash);
      EXPECT_EQ(found->token_prefix, session.token_prefix);
      EXPECT_FALSE(found->revoked_at.has_value());
      // NOLINTEND(bugprone-unchecked-optional-access)
    }

    TEST_P(SessionRepositoryTest, DuplicateIdThrowsUniqueViolation) {
      const auto session = make_session(1, user1_);
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Session>().insert(session, mutation_context());
        txn->commit();
      }
      auto txn = backend().begin(IsolationLevel::Serializable);
      EXPECT_THROW(txn->repo<core::Session>().insert(session, mutation_context()), UniqueViolation);
    }

    TEST_P(SessionRepositoryTest, DuplicateActivePrefixThrowsUniqueViolationAtCommit) {
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
        // SQLite stages and fails the partial unique index at commit; Postgres
        // fails on the immediate insert. Wrap both.
        auto txn = backend().begin(IsolationLevel::Serializable);
        EXPECT_THROW(
            {
              txn->repo<core::Session>().insert(session2, mutation_context());
              txn->commit();
            },
            UniqueViolation);
      }
    }

    TEST_P(SessionRepositoryTest, RevokedSessionAllowsSamePrefixForNewSession) {
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
        auto txn = backend().begin_auth_transaction({user1_});
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
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access): guarded by ASSERT_TRUE above
      EXPECT_FALSE(found->revoked_at.has_value());
    }

    TEST_P(SessionRepositoryTest, DefaultQueryExcludesRevokedSessions) {
      const auto session = make_session(1, user1_);
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Session>().insert(session, mutation_context());
        txn->commit();
      }
      {
        auto txn = backend().begin_auth_transaction({user1_});
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
        ASSERT_EQ(results.size(), 1U);
        EXPECT_EQ(results.front().id, session.id);
        EXPECT_TRUE(results.front().revoked_at.has_value());
      }
    }

    TEST_P(SessionRepositoryTest, SoftDeleteSetsRevokedAt) {
      const auto session = make_session(1, user1_);
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Session>().insert(session, mutation_context());
        txn->commit();
      }
      {
        auto txn = backend().begin_auth_transaction({user1_});
        txn->repo<core::Session>().soft_delete(session.id, mutation_context());
        txn->commit();
      }
      auto txn = backend().begin(IsolationLevel::Serializable);
      const auto found = txn->repo<core::Session>().find_by_id(session.id);
      ASSERT_TRUE(found.has_value());
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access): guarded by ASSERT_TRUE above
      EXPECT_TRUE(found->revoked_at.has_value());
    }

    TEST_P(SessionRepositoryTest, SoftDeleteNonExistentThrowsNotFound) {
      auto txn = backend().begin(IsolationLevel::Serializable);
      const auto fake_id = id_from_low<core::SessionId>(999);
      EXPECT_THROW(txn->repo<core::Session>().soft_delete(fake_id, mutation_context()), NotFound);
    }

    TEST_P(SessionRepositoryTest, UpdateLastSeenAt) {
      const auto session = make_session(1, user1_);
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Session>().insert(session, mutation_context());
        txn->commit();
      }
      {
        auto txn = backend().begin_auth_transaction({user1_});
        auto updated_session = session;
        updated_session.last_seen_at = ts(99999);
        txn->repo<core::Session>().update(updated_session, mutation_context());
        txn->commit();
      }
      auto txn = backend().begin(IsolationLevel::Serializable);
      const auto found = txn->repo<core::Session>().find_by_id(session.id);
      ASSERT_TRUE(found.has_value());
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access): guarded by ASSERT_TRUE above
      EXPECT_EQ(found->last_seen_at.unix_micros(), 99999);
    }

    TEST_P(SessionRepositoryTest, AuthTransactionRecordsLockedAndMissingRequestedUsers) {
      const auto missing = id_from_low<core::UserId>(900);
      const auto unrequested = id_from_low<core::UserId>(901);
      auto txn = backend().begin_auth_transaction({user2_, user1_, user2_, missing});
      EXPECT_EQ(txn->auth_user_lock_status(user1_), AuthUserLockStatus::Locked);
      EXPECT_EQ(txn->auth_user_lock_status(user2_), AuthUserLockStatus::Locked);
      EXPECT_EQ(txn->auth_user_lock_status(missing), AuthUserLockStatus::Missing);
      EXPECT_THROW((void)txn->auth_user_lock_status(unrequested), ConstraintViolation);
      EXPECT_TRUE(txn->repo<core::User>().find_by_id(user1_).has_value());
      EXPECT_FALSE(txn->repo<core::User>().find_by_id(missing).has_value());
      txn->commit();
    }

    TEST_P(SessionRepositoryTest, AuthTransactionRejectsEmptyUserList) {
      EXPECT_THROW((void)backend().begin_auth_transaction({}), ConstraintViolation);
    }

    TEST_P(SessionRepositoryTest, AuthTransactionTakesUserLockBeforeItReturns) {
      const auto audits_before = audit_event_count();
      auto first = backend().begin_auth_transaction({user1_});
      ASSERT_EQ(first->auth_user_lock_status(user1_), AuthUserLockStatus::Locked);
      const auto start = std::chrono::steady_clock::now();
      // There is no sleep or scheduling assumption: first already owns its DB
      // lock when the second, independent transaction attempts the same user.
      EXPECT_THROW((void)backend().begin_auth_transaction({user1_}), Unavailable);
      EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(10));
      first->rollback();
      EXPECT_EQ(audit_event_count(), audits_before);
      auto retry = backend().begin_auth_transaction({user1_});
      EXPECT_EQ(retry->auth_user_lock_status(user1_), AuthUserLockStatus::Locked);
      retry->commit();
    }

    TEST_P(SessionRepositoryTest, CommittedAuthTransactionCannotClaimAReleasedUserLock) {
      const auto session = make_session(900, user1_);
      auto txn = backend().begin_auth_transaction({user1_});
      txn->commit();
      EXPECT_THROW((void)txn->auth_user_lock_status(user1_), ConstraintViolation);
      EXPECT_THROW((void)txn->repo<core::Session>().find_by_id(id_from_low<core::SessionId>(900)),
                   ConstraintViolation);
      EXPECT_THROW(txn->repo<core::Session>().update(session, mutation_context()),
                   ConstraintViolation);
      EXPECT_THROW(txn->repo<core::Session>().soft_delete(session.id, mutation_context()),
                   ConstraintViolation);
      if (GetParam() == BackendKind::Postgres) {
        EXPECT_THROW(txn->set_session_var("finished", "ignored"), ConstraintViolation);
      }
    }

    TEST_P(SessionRepositoryTest, RolledBackAuthTransactionCannotClaimAReleasedUserLock) {
      const auto session = make_session(900, user1_);
      auto txn = backend().begin_auth_transaction({user1_});
      txn->rollback();
      EXPECT_THROW((void)txn->auth_user_lock_status(user1_), ConstraintViolation);
      EXPECT_THROW((void)txn->repo<core::Session>().find_by_id(id_from_low<core::SessionId>(900)),
                   ConstraintViolation);
      EXPECT_THROW(txn->repo<core::Session>().update(session, mutation_context()),
                   ConstraintViolation);
      EXPECT_THROW(txn->repo<core::Session>().soft_delete(session.id, mutation_context()),
                   ConstraintViolation);
      if (GetParam() == BackendKind::Postgres) {
        EXPECT_THROW(txn->set_session_var("finished", "ignored"), ConstraintViolation);
      }
    }

    TEST_P(SessionRepositoryTest, MissingAuthUserStatusCannotBecomeALockAfterInsert) {
      const auto missing = id_from_low<core::UserId>(900);
      auto txn = backend().begin_auth_transaction({missing});
      EXPECT_EQ(txn->auth_user_lock_status(missing), AuthUserLockStatus::Missing);
      txn->repo<core::User>().insert(make_user(900, lab1_), mutation_context());
      EXPECT_TRUE(txn->repo<core::User>().find_by_id(missing).has_value());
      EXPECT_EQ(txn->auth_user_lock_status(missing), AuthUserLockStatus::Missing);
      const auto session = make_session(901, missing);
      txn->repo<core::Session>().insert(session, mutation_context());
      EXPECT_THROW(txn->repo<core::Session>().update(session, mutation_context()),
                   ConstraintViolation);
      txn->rollback();
    }

    TEST_P(SessionRepositoryTest, OrdinaryTransactionCannotClaimAnAuthUserLock) {
      auto txn = backend().begin(IsolationLevel::Serializable);
      EXPECT_THROW((void)txn->auth_user_lock_status(user1_), ConstraintViolation);
    }

    TEST_P(SessionRepositoryTest, SessionUpdateRequiresItsOwningUserLock) {
      auto session = make_session(1, user1_);
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Session>().insert(session, mutation_context());
        txn->commit();
      }
      const auto audits_before = audit_event_count();
      session.last_seen_at = ts(99'999);
      {
        auto ordinary = backend().begin(IsolationLevel::Serializable);
        EXPECT_THROW(ordinary->repo<core::Session>().update(session, mutation_context()),
                     ConstraintViolation);
      }
      {
        auto wrong_owner = backend().begin_auth_transaction({user2_});
        EXPECT_THROW(wrong_owner->repo<core::Session>().update(session, mutation_context()),
                     ConstraintViolation);
      }
      auto read = backend().begin(IsolationLevel::ReadCommitted);
      EXPECT_EQ(read->repo<core::Session>().find_by_id(session.id)->last_seen_at, ts(2001));
      read->commit();
      EXPECT_EQ(audit_event_count(), audits_before);
    }

    TEST_P(SessionRepositoryTest, SessionRevocationRequiresItsOwningUserLock) {
      const auto session = make_session(1, user1_);
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Session>().insert(session, mutation_context());
        txn->commit();
      }
      const auto audits_before = audit_event_count();
      {
        auto ordinary = backend().begin(IsolationLevel::Serializable);
        EXPECT_THROW(ordinary->repo<core::Session>().soft_delete(session.id, mutation_context()),
                     ConstraintViolation);
      }
      {
        auto wrong_owner = backend().begin_auth_transaction({user2_});
        EXPECT_THROW(wrong_owner->repo<core::Session>().soft_delete(session.id, mutation_context()),
                     ConstraintViolation);
      }
      auto read = backend().begin(IsolationLevel::ReadCommitted);
      EXPECT_FALSE(read->repo<core::Session>().find_by_id(session.id)->revoked_at.has_value());
      read->commit();
      EXPECT_EQ(audit_event_count(), audits_before);
    }

    TEST_P(SessionRepositoryTest, LockedSessionUpdateCannotChangeItsOwningUser) {
      auto session = make_session(1, user1_);
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Session>().insert(session, mutation_context());
        txn->commit();
      }
      const auto audits_before = audit_event_count();
      {
        auto txn = backend().begin_auth_transaction({user1_, user2_});
        session.user_id = user2_;
        EXPECT_THROW(txn->repo<core::Session>().update(session, mutation_context()),
                     ConstraintViolation);
      }
      auto read = backend().begin(IsolationLevel::ReadCommitted);
      EXPECT_EQ(read->repo<core::Session>().find_by_id(session.id)->user_id, user1_);
      read->commit();
      EXPECT_EQ(audit_event_count(), audits_before);
    }

    TEST_P(SessionRepositoryTest, AuthRollbackDiscardsSessionUpdateAndReleasesLocks) {
      const auto session = make_session(1, user1_);
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Session>().insert(session, mutation_context());
        txn->commit();
      }
      const auto audits_before = audit_event_count();
      auto txn = backend().begin_auth_transaction({user1_});
      auto changed = txn->repo<core::Session>().find_by_id(session.id).value();
      changed.last_seen_at = ts(99'999);
      txn->repo<core::Session>().update(changed, mutation_context());
      EXPECT_EQ(txn->repo<core::Session>().find_by_id(session.id)->last_seen_at, ts(99'999));
      txn->rollback();
      // Keep the rolled-back object alive: rollback itself must release its DB lock.
      auto next = backend().begin_auth_transaction({user1_});
      EXPECT_EQ(next->repo<core::Session>().find_by_id(session.id)->last_seen_at, ts(2001));
      next->commit();
      EXPECT_EQ(audit_event_count(), audits_before);
    }

    TEST_P(SessionRepositoryTest, AuthDestructorDiscardsSessionUpdateAndReleasesLocks) {
      const auto session = make_session(1, user1_);
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Session>().insert(session, mutation_context());
        txn->commit();
      }
      const auto audits_before = audit_event_count();
      {
        auto txn = backend().begin_auth_transaction({user1_});
        auto changed = txn->repo<core::Session>().find_by_id(session.id).value();
        changed.last_seen_at = ts(99'999);
        txn->repo<core::Session>().update(changed, mutation_context());
      }
      auto next = backend().begin_auth_transaction({user1_});
      EXPECT_EQ(next->repo<core::Session>().find_by_id(session.id)->last_seen_at, ts(2001));
      next->commit();
      EXPECT_EQ(audit_event_count(), audits_before);
    }

    TEST_P(SessionRepositoryTest, AuthCommitFailureRollsBackSessionAuditAndReleasesLocks) {
      const auto session = make_session(1, user1_);
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Session>().insert(session, mutation_context());
        txn->commit();
      }
      const auto audits_before = audit_event_count();
      auto txn = backend().begin_auth_transaction({user1_});
      auto changed = txn->repo<core::Session>().find_by_id(session.id).value();
      changed.last_seen_at = ts(99'999);
      changed.mfa_complete = false;
      txn->repo<core::Session>().update(changed, mutation_context());
      fail_next_audit_append();
      EXPECT_THROW(txn->commit(), ConstraintViolation);
      EXPECT_THROW((void)txn->auth_user_lock_status(user1_), ConstraintViolation);
      EXPECT_THROW((void)txn->repo<core::Session>().find_by_id(session.id), ConstraintViolation);
      EXPECT_THROW(txn->repo<core::Session>().update(session, mutation_context()),
                   ConstraintViolation);
      EXPECT_THROW(txn->repo<core::Session>().soft_delete(session.id, mutation_context()),
                   ConstraintViolation);
      if (GetParam() == BackendKind::Postgres) {
        EXPECT_THROW(txn->set_session_var("finished", "ignored"), ConstraintViolation);
      }
      // The failed transaction is still alive; failure itself must release locks.
      auto next = backend().begin_auth_transaction({user1_});
      const auto after = next->repo<core::Session>().find_by_id(session.id).value();
      EXPECT_EQ(after.last_seen_at, session.last_seen_at);
      EXPECT_EQ(after.mfa_complete, session.mfa_complete);
      EXPECT_EQ(after.revoked_at, session.revoked_at);
      next->commit();
      EXPECT_EQ(audit_event_count(), audits_before);
    }

    TEST_P(SessionRepositoryTest, DelayedProviderRefreshPreservesCommittedRevocation) {
      const auto token = login_for_activity_test(false);
      auth::LocalAuthProvider provider(backend(), auth_config());
      PausedActivityBackend paused(backend());
      auth::LocalAuthProvider delayed(paused, auth_config());
      auto validation = std::async(std::launch::async,
                                   [&] { return delayed.validate_token(token.plaintext_token); });
      const bool reached = paused.wait_until_paused();
      if (reached) {
        EXPECT_NO_THROW(provider.revoke_session(token.session_id, mutation_context()));
      }
      const auto audits_after_revoke = audit_event_count();
      paused.release();
      EXPECT_NO_THROW((void)validation.get());
      ASSERT_TRUE(reached);
      EXPECT_TRUE(read_session(token.session_id).revoked_at.has_value());
      EXPECT_EQ(audit_event_count(), audits_after_revoke);
      EXPECT_THROW(provider.validate_token(token.plaintext_token), auth::InvalidCredentials);
      EXPECT_THROW(provider.revalidate_token(token.plaintext_token), auth::InvalidCredentials);
    }

    TEST_P(SessionRepositoryTest, DelayedProviderRefreshPreservesCommittedMfaCompletion) {
      const auto token = login_for_activity_test(true);
      auth::LocalAuthProvider provider(backend(), auth_config());
      PausedActivityBackend paused(backend());
      auth::LocalAuthProvider delayed(paused, auth_config());
      auto validation = std::async(std::launch::async,
                                   [&] { return delayed.validate_token(token.plaintext_token); });
      const bool reached = paused.wait_until_paused();
      if (reached) {
        EXPECT_NO_THROW(complete_mfa(provider, token.session_id));
      }
      paused.release();
      EXPECT_NO_THROW((void)validation.get());
      ASSERT_TRUE(reached);
      EXPECT_TRUE(read_session(token.session_id).mfa_complete);
      EXPECT_TRUE(provider.revalidate_token(token.plaintext_token).mfa_complete);
    }

    TEST_P(SessionRepositoryTest, ProviderRefreshBeforeRevocationCannotRestoreAuthority) {
      const auto token = login_for_activity_test(false);
      auth::LocalAuthProvider provider(backend(), auth_config());
      EXPECT_NO_THROW((void)provider.validate_token(token.plaintext_token));
      provider.revoke_session(token.session_id, mutation_context());
      EXPECT_TRUE(read_session(token.session_id).revoked_at.has_value());
      EXPECT_THROW(provider.validate_token(token.plaintext_token), auth::InvalidCredentials);
      EXPECT_THROW(provider.revalidate_token(token.plaintext_token), auth::InvalidCredentials);
    }

    TEST_P(SessionRepositoryTest, ProviderRefreshBeforeMfaCompletionPreservesCompletion) {
      const auto token = login_for_activity_test(true);
      auth::LocalAuthProvider provider(backend(), auth_config());
      EXPECT_FALSE(provider.validate_token(token.plaintext_token).mfa_complete);
      complete_mfa(provider, token.session_id);
      EXPECT_TRUE(read_session(token.session_id).mfa_complete);
      EXPECT_TRUE(provider.revalidate_token(token.plaintext_token).mfa_complete);
    }

    TEST_P(SessionRepositoryTest, DelayedProviderRefreshUsesLiveTimestampWithoutRedundantAudit) {
      const auto token = login_for_activity_test(false);
      PausedActivityBackend paused(backend());
      auth::LocalAuthProvider delayed(paused, auth_config());
      auto validation = std::async(std::launch::async,
                                   [&] { return delayed.validate_token(token.plaintext_token); });
      const bool reached = paused.wait_until_paused();
      const auto newer = core::Timestamp::from_unix_micros(now_micros() + 1'000'000);
      if (reached) {
        auto txn = backend().begin_auth_transaction({user1_});
        auto session = txn->repo<core::Session>().find_by_id(token.session_id).value();
        session.last_seen_at = newer;
        txn->repo<core::Session>().update(session, mutation_context());
        txn->commit();
      }
      const auto audits_before_release = audit_event_count();
      paused.release();
      EXPECT_NO_THROW((void)validation.get());
      ASSERT_TRUE(reached);
      EXPECT_EQ(read_session(token.session_id).last_seen_at, newer);
      EXPECT_EQ(audit_event_count(), audits_before_release);
    }

    TEST_P(SessionRepositoryTest, ConcurrentProviderRefreshesRecomputeActivityEligibility) {
      const auto token = login_for_activity_test(false);
      PausedActivityBackend paused_first(backend());
      PausedActivityBackend paused_second(backend());
      auth::LocalAuthProvider first(paused_first, auth_config());
      auth::LocalAuthProvider second(paused_second, auth_config());
      auto first_result = std::async(std::launch::async,
                                     [&] { return first.validate_token(token.plaintext_token); });
      auto second_result = std::async(std::launch::async,
                                      [&] { return second.validate_token(token.plaintext_token); });
      const bool first_reached = paused_first.wait_until_paused();
      const bool second_reached = paused_second.wait_until_paused();
      const auto audits_before = audit_event_count();
      paused_first.release();
      EXPECT_NO_THROW((void)first_result.get());
      const auto first_activity = read_session(token.session_id).last_seen_at;
      paused_second.release();
      EXPECT_NO_THROW((void)second_result.get());
      ASSERT_TRUE(first_reached);
      ASSERT_TRUE(second_reached);
      EXPECT_EQ(read_session(token.session_id).last_seen_at, first_activity);
      EXPECT_EQ(audit_event_count(), audits_before + 1);
    }

    TEST_P(SessionRepositoryTest, ProviderActivityAuditFailureRollsBackAndDoesNotBlockValidation) {
      const auto token = login_for_activity_test(true);
      auth::LocalAuthProvider provider(backend(), auth_config());
      complete_mfa(provider, token.session_id);
      const auto before = read_session(token.session_id);
      const auto audits_before = audit_event_count();
      fail_next_audit_append();
      EXPECT_TRUE(provider.validate_token(token.plaintext_token).mfa_complete);
      const auto after = read_session(token.session_id);
      EXPECT_EQ(after.last_seen_at, before.last_seen_at);
      EXPECT_EQ(after.revoked_at, before.revoked_at);
      EXPECT_EQ(after.mfa_complete, before.mfa_complete);
      EXPECT_EQ(audit_event_count(), audits_before);
      EXPECT_TRUE(provider.validate_token(token.plaintext_token).mfa_complete);
      EXPECT_GT(read_session(token.session_id).last_seen_at, before.last_seen_at);
      EXPECT_EQ(audit_event_count(), audits_before + 1);
    }

    TEST_P(SessionRepositoryTest, PasswordSessionCreationRechecksCurrentEnrollmentUnderLock) {
      const auto existing = login_for_activity_test(false);
      PausedActivityBackend paused(backend());
      auth::LocalAuthProvider delayed(paused, auth_config());
      auto login = std::async(std::launch::async, [&] {
        return delayed.authenticate(auth::PasswordCredentials{.email = "user10@example.org",
                                                              .password = "fixture-password"},
                                    auth::ClientInfo{});
      });
      const bool reached = paused.wait_until_paused();
      if (reached) {
        auto write = backend().begin_auth_transaction({user1_});
        write->replace_totp_secret(user1_, std::string(k_totp_secret), mutation_context());
        write->commit();
      }
      paused.release();
      const auto created = login.get();
      ASSERT_TRUE(reached);
      EXPECT_NE(created.session_id, existing.session_id);
      EXPECT_FALSE(created.mfa_complete);
      EXPECT_FALSE(read_session(created.session_id).mfa_complete);
    }

    TEST_P(SessionRepositoryTest, PasswordSessionCreationRejectsDisabledUserOrderedBeforeLock) {
      const auto existing = login_for_activity_test(false);
      PausedActivityBackend paused(backend());
      auth::LocalAuthProvider delayed(paused, auth_config());
      auto login = std::async(std::launch::async, [&] {
        return delayed.authenticate(auth::PasswordCredentials{.email = "user10@example.org",
                                                              .password = "fixture-password"},
                                    auth::ClientInfo{});
      });
      const bool reached = paused.wait_until_paused();
      if (reached) {
        auto write = backend().begin(IsolationLevel::Serializable);
        auto user = write->repo<core::User>().find_by_id(user1_).value();
        user.status = core::UserStatus::Disabled;
        write->repo<core::User>().update(user, mutation_context());
        write->commit();
      }
      const auto audits_before_release = audit_event_count();
      paused.release();
      EXPECT_THROW((void)login.get(), auth::InvalidCredentials);
      ASSERT_TRUE(reached);
      auto read = backend().begin(IsolationLevel::ReadCommitted);
      const auto sessions = read->repo<core::Session>().query(Query<core::Session>::all());
      read->commit();
      ASSERT_EQ(sessions.size(), 1U);
      EXPECT_EQ(sessions.front().id, existing.session_id);
      EXPECT_EQ(audit_event_count(), audits_before_release);
    }

    TEST_P(SessionRepositoryTest, PasswordBindingChangeCannotCreateSessionOrClearFailureBudget) {
      const auto existing = login_for_activity_test(false);
      auth::LocalAuthProvider provider(backend(), auth_config());
      PausedActivityBackend paused(backend());
      auto config = auth_config();
      config.max_failures_before_lockout = 2;
      auth::LocalAuthProvider delayed(paused, config);
      EXPECT_THROW(
          (void)delayed.authenticate(auth::PasswordCredentials{.email = "user10@example.org",
                                                               .password = "wrong-password"},
                                     auth::ClientInfo{}),
          auth::InvalidCredentials);
      auto login = std::async(std::launch::async, [&] {
        return delayed.authenticate(auth::PasswordCredentials{.email = "user10@example.org",
                                                              .password = "fixture-password"},
                                    auth::ClientInfo{});
      });
      const bool reached = paused.wait_until_paused();
      if (reached) {
        auto write = backend().begin(IsolationLevel::Serializable);
        auto user = write->repo<core::User>().find_by_id(user1_).value();
        user.auth_bindings = nlohmann::json::array(
            {{{"provider", "local"}, {"hash", provider.hash_password("replacement-password")}}});
        write->repo<core::User>().update(user, mutation_context());
        write->commit();
      }
      paused.release();
      EXPECT_THROW((void)login.get(), auth::InvalidCredentials);
      ASSERT_TRUE(reached);
      EXPECT_THROW(
          (void)delayed.authenticate(auth::PasswordCredentials{.email = "user10@example.org",
                                                               .password = "wrong-password"},
                                     auth::ClientInfo{}),
          auth::AccountLocked);
      auto read = backend().begin(IsolationLevel::ReadCommitted);
      const auto sessions = read->repo<core::Session>().query(Query<core::Session>::all());
      read->commit();
      ASSERT_EQ(sessions.size(), 1U);
      EXPECT_EQ(sessions.front().id, existing.session_id);
    }

    TEST_P(SessionRepositoryTest, QueryByUserId) {
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
      ASSERT_EQ(results.size(), 1U);
      EXPECT_EQ(results.front().id, s1.id);
    }

    TEST_P(SessionRepositoryTest, QueryByTokenPrefix) {
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
      ASSERT_EQ(results.size(), 1U);
      EXPECT_EQ(results.front().id, session.id);
    }

    TEST_P(SessionRepositoryTest, EmptyTokenHashThrowsConstraintViolation) {
      auto session = make_session(1, user1_);
      session.token_hash = "";
      auto txn = backend().begin(IsolationLevel::Serializable);
      EXPECT_THROW(txn->repo<core::Session>().insert(session, mutation_context()),
                   ConstraintViolation);
    }

    TEST_P(SessionRepositoryTest, EmptyTokenPrefixThrowsConstraintViolation) {
      auto session = make_session(1, user1_);
      session.token_prefix = "";
      auto txn = backend().begin(IsolationLevel::Serializable);
      EXPECT_THROW(txn->repo<core::Session>().insert(session, mutation_context()),
                   ConstraintViolation);
    }

    // ---- ApiToken tests ----

    TEST_P(SessionRepositoryTest, ApiTokenInsertAndFindById) {
      const auto token = make_api_token(1, user1_, lab1_);
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::ApiToken>().insert(token, mutation_context());
        txn->commit();
      }
      auto txn = backend().begin(IsolationLevel::Serializable);
      const auto found = txn->repo<core::ApiToken>().find_by_id(token.id);
      ASSERT_TRUE(found.has_value());
      // NOLINTBEGIN(bugprone-unchecked-optional-access): guarded by ASSERT_TRUE above
      EXPECT_EQ(found->id, token.id);
      EXPECT_EQ(found->user_id, user1_);
      ASSERT_TRUE(found->lab_id.has_value());
      EXPECT_EQ(*found->lab_id, lab1_);
      EXPECT_EQ(found->name, token.name);
      EXPECT_FALSE(found->revoked_at.has_value());
      // NOLINTEND(bugprone-unchecked-optional-access)
    }

    TEST_P(SessionRepositoryTest, ApiTokenWithoutLabId) {
      const auto token = make_api_token(2, user1_, std::nullopt);
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::ApiToken>().insert(token, mutation_context());
        txn->commit();
      }
      auto txn = backend().begin(IsolationLevel::Serializable);
      const auto found = txn->repo<core::ApiToken>().find_by_id(token.id);
      ASSERT_TRUE(found.has_value());
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access): guarded by ASSERT_TRUE above
      EXPECT_FALSE(found->lab_id.has_value());
    }

    TEST_P(SessionRepositoryTest, ApiTokenSoftDeleteSetsRevokedAt) {
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
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access): guarded by ASSERT_TRUE above
      EXPECT_TRUE(found->revoked_at.has_value());
    }

    TEST_P(SessionRepositoryTest, ApiTokenDefaultQueryExcludesRevoked) {
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
        ASSERT_EQ(results.size(), 1U);
        EXPECT_EQ(results.front().id, token.id);
        EXPECT_TRUE(results.front().revoked_at.has_value());
      }
    }

    TEST_P(SessionRepositoryTest, ApiTokenDuplicateActivePrefixThrowsUniqueViolationAtCommit) {
      const auto token1 = make_api_token(1, user1_, lab1_);
      core::ApiToken token2 = make_api_token(2, user2_, std::nullopt);
      token2.token_prefix = token1.token_prefix; // collision
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::ApiToken>().insert(token1, mutation_context());
        txn->commit();
      }
      {
        // SQLite stages and fails the partial unique index at commit; Postgres
        // fails on the immediate insert. Wrap both.
        auto txn = backend().begin(IsolationLevel::Serializable);
        EXPECT_THROW(
            {
              txn->repo<core::ApiToken>().insert(token2, mutation_context());
              txn->commit();
            },
            UniqueViolation);
      }
    }

    TEST_P(SessionRepositoryTest, ApiTokenEmptyNameThrowsConstraintViolation) {
      auto token = make_api_token(1, user1_, std::nullopt);
      token.name = "";
      auto txn = backend().begin(IsolationLevel::Serializable);
      EXPECT_THROW(txn->repo<core::ApiToken>().insert(token, mutation_context()),
                   ConstraintViolation);
    }

    TEST_P(SessionRepositoryTest, ApiTokenQueryByUserId) {
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
      ASSERT_EQ(results.size(), 1U);
      EXPECT_EQ(results.front().id, t1.id);
    }

    TEST_P(SessionRepositoryTest, ApiTokenMutationsAreAudited) {
      const auto token = make_api_token(1, user1_, lab1_);
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::ApiToken>().insert(token, mutation_context());
        txn->commit();
      }
      // At least 1 audit row should have been inserted.
      EXPECT_GE(audit_event_count(), 1U);
    }

    TEST_P(SessionRepositoryTest, SessionUpdateNonExistentThrows) {
      const auto session = make_session(99999, user1_);
      auto txn = backend().begin(IsolationLevel::Serializable);
      EXPECT_THROW(txn->repo<core::Session>().update(session, mutation_context()), NotFound);
    }

    TEST_P(SessionRepositoryTest, ApiTokenUpdateNonExistentThrows) {
      const auto token = make_api_token(99999, user1_, lab1_);
      auto txn = backend().begin(IsolationLevel::Serializable);
      EXPECT_THROW(txn->repo<core::ApiToken>().update(token, mutation_context()), NotFound);
    }

    TEST_P(SessionRepositoryTest, GenericUserInsertRejectsTotpSecret) {
      auto user = make_user(30, lab1_);
      user.totp_secret_enc = "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ";
      auto txn = backend().begin(IsolationLevel::Serializable);
      EXPECT_THROW(txn->repo<core::User>().insert(user, mutation_context()), ConstraintViolation);
    }

    TEST_P(SessionRepositoryTest, GenericUserUpdateRejectsTotpSecretChange) {
      auto txn = backend().begin_auth_transaction({user1_});
      auto user = txn->repo<core::User>().find_by_id(user1_).value();
      user.totp_secret_enc = "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ";
      EXPECT_THROW(txn->repo<core::User>().update(user, mutation_context()), ConstraintViolation);
    }

    TEST_P(SessionRepositoryTest, MfaStateInitializationIsLockedAuditedAndReadYourWrites) {
      const auto count = audit_event_count();
      auto txn = backend().begin_auth_transaction({user1_});
      const auto first = txn->initialize_mfa_state(user1_, mutation_context());
      EXPECT_EQ(first.user_id, user1_);
      EXPECT_EQ(first.failure_count, 0);
      EXPECT_FALSE(first.last_accepted_step);
      EXPECT_FALSE(first.totp_generation);
      EXPECT_EQ(txn->initialize_mfa_state(user1_, mutation_context()), first);
      EXPECT_EQ(txn->repo<core::MfaSecurityState>().find_by_id(user1_), first);
      EXPECT_EQ(
          txn->repo<core::MfaSecurityState>().query(Query<core::MfaSecurityState>::all()).size(),
          1U);
      txn->commit();
      EXPECT_EQ(audit_event_count(), count + 1);
    }

    TEST_P(SessionRepositoryTest, MfaStateInitializationOutsideOwnerLockFailsClosed) {
      auto txn = backend().begin(IsolationLevel::Serializable);
      EXPECT_THROW((void)txn->initialize_mfa_state(user1_, mutation_context()), BackendError);
      auto missing = backend().begin_auth_transaction({id_from_low<core::UserId>(9999)});
      EXPECT_THROW(
          (void)missing->initialize_mfa_state(id_from_low<core::UserId>(9999), mutation_context()),
          BackendError);
    }

    TEST_P(SessionRepositoryTest,
           MfaStateGenericWritesCannotCreateEraseChangeGenerationOrLowerFloor) {
      auto txn = backend().begin_auth_transaction({user1_});
      auto state = txn->initialize_mfa_state(user1_, mutation_context());
      EXPECT_THROW(txn->repo<core::MfaSecurityState>().insert(state, mutation_context()),
                   ConstraintViolation);
      state.last_accepted_step = 10;
      state.observed_at = ts(100);
      state.failure_count = 7;
      state.next_allowed_at = ts(200);
      txn->repo<core::MfaSecurityState>().update(state, mutation_context());
      EXPECT_EQ(txn->repo<core::MfaSecurityState>().find_by_id(user1_), state);
      auto bad = state;
      bad.last_accepted_step = 9;
      EXPECT_THROW(txn->repo<core::MfaSecurityState>().update(bad, mutation_context()),
                   ConstraintViolation);
      bad = state;
      bad.last_accepted_step.reset();
      EXPECT_THROW(txn->repo<core::MfaSecurityState>().update(bad, mutation_context()),
                   ConstraintViolation);
      bad = state;
      bad.totp_generation = core::Uuid::parse(core::generate_uuid_v4());
      EXPECT_THROW(txn->repo<core::MfaSecurityState>().update(bad, mutation_context()),
                   ConstraintViolation);
      bad = state;
      bad.failure_count = 18;
      EXPECT_THROW(txn->repo<core::MfaSecurityState>().update(bad, mutation_context()),
                   ConstraintViolation);
      EXPECT_THROW(txn->repo<core::MfaSecurityState>().soft_delete(user1_, mutation_context()),
                   ConstraintViolation);
      txn->commit();
    }

    TEST_P(SessionRepositoryTest, TotpLifecycleRotatesSameValueRevokesAndPreservesReplayBudget) {
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Session>().insert(make_session(1, user1_), mutation_context());
        auto pending = make_session(2, user1_);
        pending.mfa_complete = false;
        txn->repo<core::Session>().insert(pending, mutation_context());
        txn->repo<core::Session>().insert(make_session(3, user2_), mutation_context());
        txn->commit();
      }
      const auto count = audit_event_count();
      auto txn = backend().begin_auth_transaction({user1_});
      txn->replace_totp_secret(user1_, std::string(k_totp_secret), mutation_context());
      auto state = txn->repo<core::MfaSecurityState>().find_by_id(user1_).value();
      ASSERT_TRUE(state.totp_generation);
      const auto generation = state.totp_generation;
      state.last_accepted_step = 42;
      state.observed_at = ts(100);
      state.failure_count = 17;
      state.next_allowed_at = ts(200);
      txn->repo<core::MfaSecurityState>().update(state, mutation_context());
      txn->replace_totp_secret(user1_, std::string(k_totp_secret), mutation_context());
      auto changed = txn->repo<core::MfaSecurityState>().find_by_id(user1_).value();
      EXPECT_NE(changed.totp_generation, generation);
      EXPECT_EQ(changed.last_accepted_step, state.last_accepted_step);
      EXPECT_EQ(changed.failure_count, 17);
      EXPECT_EQ(changed.next_allowed_at, state.next_allowed_at);
      EXPECT_TRUE(
          txn->repo<core::Session>().find_by_id(id_from_low<core::SessionId>(1))->revoked_at);
      EXPECT_TRUE(
          txn->repo<core::Session>().find_by_id(id_from_low<core::SessionId>(2))->revoked_at);
      EXPECT_FALSE(
          txn->repo<core::Session>().find_by_id(id_from_low<core::SessionId>(3))->revoked_at);
      txn->commit();
      // initialization + two secret/generation pairs + two revokes + state update.
      EXPECT_EQ(audit_event_count(), count + 8);
    }

    TEST_P(SessionRepositoryTest, TotpLifecycleAuditFailureRollsBackSecretStateAndRevocation) {
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Session>().insert(make_session(1, user1_), mutation_context());
        txn->commit();
      }
      const auto count = audit_event_count();
      auto txn = backend().begin_auth_transaction({user1_});
      txn->replace_totp_secret(user1_, std::string(k_totp_secret), mutation_context());
      fail_next_audit_append();
      EXPECT_THROW(txn->commit(), BackendError);
      EXPECT_THROW((void)txn->repo<core::MfaSecurityState>().find_by_id(user1_), BackendError);
      EXPECT_THROW((void)txn->repo<core::User>().find_by_id(user1_), BackendError);
      auto read = backend().begin(IsolationLevel::Serializable);
      EXPECT_FALSE(read->repo<core::User>().find_by_id(user1_)->totp_secret_enc);
      EXPECT_FALSE(read->repo<core::MfaSecurityState>().find_by_id(user1_));
      EXPECT_FALSE(
          read->repo<core::Session>().find_by_id(id_from_low<core::SessionId>(1))->revoked_at);
      EXPECT_EQ(audit_event_count(), count);
    }

    TEST_P(SessionRepositoryTest, TotpLifecycleProfileWritesCannotRestoreAStaleSecret) {
      auto txn = backend().begin_auth_transaction({user1_});
      auto stale = txn->repo<core::User>().find_by_id(user1_).value();
      stale.display_name = "profile before enrollment";
      txn->repo<core::User>().update(stale, mutation_context());
      txn->replace_totp_secret(user1_, std::string(k_totp_secret), mutation_context());
      EXPECT_EQ(txn->repo<core::User>().find_by_id(user1_)->totp_secret_enc, k_totp_secret);
      EXPECT_THROW(txn->repo<core::User>().update(stale, mutation_context()), ConstraintViolation);
      txn->commit();
      auto read = backend().begin(IsolationLevel::Serializable);
      EXPECT_EQ(read->repo<core::User>().find_by_id(user1_)->totp_secret_enc, k_totp_secret);
      EXPECT_EQ(read->repo<core::User>().find_by_id(user1_)->display_name, stale.display_name);
    }

    TEST_P(SessionRepositoryTest,
           TotpProofRequiresCurrentGenerationAndCannotBeForgedByGenericWrites) {
      core::Uuid generation;
      {
        auto txn = backend().begin_auth_transaction({user1_});
        txn->replace_totp_secret(user1_, std::string(k_totp_secret), mutation_context());
        generation =
            txn->repo<core::MfaSecurityState>().find_by_id(user1_)->totp_generation.value();
        txn->commit();
      }
      auto pending = make_session(1, user1_);
      pending.mfa_complete = false;
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Session>().insert(pending, mutation_context());
        auto forged = make_session(2, user1_);
        forged.totp_verified_generation = generation;
        forged.totp_verified_at = ts(5000);
        EXPECT_THROW(txn->repo<core::Session>().insert(forged, mutation_context()),
                     ConstraintViolation);
        txn->commit();
      }
      auto txn = backend().begin_auth_transaction({user1_});
      auto forged = pending;
      forged.mfa_complete = true;
      forged.totp_verified_generation = generation;
      forged.totp_verified_at = ts(5000);
      EXPECT_THROW(txn->repo<core::Session>().update(forged, mutation_context()),
                   ConstraintViolation);
      EXPECT_THROW(
          txn->complete_totp_session(pending.id, core::Uuid{}, ts(5000), mutation_context()),
          ConstraintViolation);
      txn->complete_totp_session(pending.id, generation, ts(5000), mutation_context());
      auto complete = txn->repo<core::Session>().find_by_id(pending.id).value();
      EXPECT_TRUE(complete.mfa_complete);
      EXPECT_EQ(complete.totp_verified_generation, generation);
      EXPECT_EQ(complete.totp_verified_at, ts(5000));
      EXPECT_EQ(complete.last_seen_at, ts(5000));
      complete.last_seen_at = ts(6000);
      txn->repo<core::Session>().update(complete, mutation_context());
      txn->repo<core::Session>().soft_delete(pending.id, mutation_context());
      EXPECT_EQ(txn->repo<core::Session>().find_by_id(pending.id)->totp_verified_generation,
                generation);
      EXPECT_THROW(txn->complete_totp_session(pending.id, generation, ts(7000), mutation_context()),
                   ConstraintViolation);
      txn->commit();
    }

    TEST_P(SessionRepositoryTest, MfaStateConcurrentMissingInitializationCommitsExactlyOnce) {
      const auto count = audit_event_count();
      auto first = backend().begin_auth_transaction({user1_});
      const auto state = first->initialize_mfa_state(user1_, mutation_context());
      std::promise<void> started;
      auto entered = started.get_future();
      auto second = std::async(std::launch::async, [&] {
        started.set_value();
        auto txn = backend().begin_auth_transaction({user1_});
        const auto existing = txn->initialize_mfa_state(user1_, mutation_context());
        txn->commit();
        return existing;
      });
      const bool contender_started =
          entered.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
      const bool blocked_while_locked =
          second.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout;
      first->commit();
      ASSERT_TRUE(contender_started);
      EXPECT_TRUE(blocked_while_locked);
      ASSERT_EQ(second.wait_for(std::chrono::seconds(5)), std::future_status::ready);
      EXPECT_EQ(second.get(), state);
      auto read = backend().begin(IsolationLevel::ReadCommitted);
      EXPECT_EQ(read->repo<core::MfaSecurityState>().find_by_id(user1_), state);
      EXPECT_EQ(
          read->repo<core::MfaSecurityState>().query(Query<core::MfaSecurityState>::all()).size(),
          1U);
      read->commit();
      EXPECT_EQ(audit_event_count(), count + 1);
      EXPECT_TRUE(audit::verify_audit_chain(audit_events()).ok);
    }

    TEST_P(SessionRepositoryTest, MfaStateInitializationRollbackAndTerminalAccessFailClosed) {
      const auto count = audit_event_count();
      {
        auto txn = backend().begin_auth_transaction({user1_});
        (void)txn->initialize_mfa_state(user1_, mutation_context());
        txn->rollback();
        EXPECT_THROW((void)txn->initialize_mfa_state(user1_, mutation_context()), BackendError);
        EXPECT_THROW(
            (void)txn->repo<core::MfaSecurityState>().query(Query<core::MfaSecurityState>::all()),
            BackendError);
        EXPECT_THROW(
            txn->replace_totp_secret(user1_, std::string(k_totp_secret), mutation_context()),
            BackendError);
      }
      {
        auto txn = backend().begin_auth_transaction({user1_});
        (void)txn->initialize_mfa_state(user1_, mutation_context());
        fail_next_audit_append();
        EXPECT_THROW(txn->commit(), BackendError);
        EXPECT_THROW((void)txn->repo<core::MfaSecurityState>().find_by_id(user1_), BackendError);
      }
      auto read = backend().begin(IsolationLevel::ReadCommitted);
      EXPECT_FALSE(read->repo<core::MfaSecurityState>().find_by_id(user1_));
      read->commit();
      EXPECT_EQ(audit_event_count(), count);
    }

    TEST_P(SessionRepositoryTest, MfaStateGenericUpdateRequiresOwnerAndPreservesObservedHighWater) {
      core::MfaSecurityState state;
      {
        auto txn = backend().begin_auth_transaction({user1_});
        state = txn->initialize_mfa_state(user1_, mutation_context());
        state.last_accepted_step = 50;
        state.observed_at = ts(100);
        txn->repo<core::MfaSecurityState>().update(state, mutation_context());
        txn->commit();
      }
      {
        auto txn = backend().begin(IsolationLevel::ReadCommitted);
        EXPECT_THROW(txn->repo<core::MfaSecurityState>().update(state, mutation_context()),
                     BackendError);
      }
      auto txn = backend().begin_auth_transaction({user1_});
      auto regressed = state;
      regressed.observed_at = ts(99);
      EXPECT_THROW(txn->repo<core::MfaSecurityState>().update(regressed, mutation_context()),
                   ConstraintViolation);
      regressed.observed_at.reset();
      EXPECT_THROW(txn->repo<core::MfaSecurityState>().update(regressed, mutation_context()),
                   ConstraintViolation);
      EXPECT_EQ(txn->repo<core::MfaSecurityState>().find_by_id(user1_), state);
      txn->commit();
    }

    TEST_P(SessionRepositoryTest,
           TotpLifecycleRevokesStagedInsertsOnlyOnceAndAuditsExactSafeActions) {
      auto txn = backend().begin_auth_transaction({user1_});
      auto first = make_session(91, user1_);
      first.token_hash = "test-only-private-token-sentinel";
      first.token_prefix = "test-only-private-prefix";
      auto second = make_session(92, user1_);
      second.mfa_complete = false;
      txn->repo<core::Session>().insert(first, mutation_context());
      txn->repo<core::Session>().insert(second, mutation_context());
      auto ctx = mutation_context();
      ctx.request_id = "lifecycle-exact-audit";
      txn->replace_totp_secret(user1_, std::string(k_totp_secret), ctx);
      const auto first_generation =
          txn->repo<core::MfaSecurityState>().find_by_id(user1_)->totp_generation;
      txn->replace_totp_secret(user1_, std::string(k_totp_secret), ctx);
      const auto last_generation =
          txn->repo<core::MfaSecurityState>().find_by_id(user1_)->totp_generation;
      EXPECT_NE(first_generation, last_generation);
      txn->commit();
      const auto events = audit_events();
      std::multiset<std::string> actions;
      const std::set<std::string> secret_keys{"enrolled", "generation"};
      const std::set<std::string> state_keys{"user_id",       "last_accepted_step",
                                             "failure_count", "next_allowed_at",
                                             "observed_at",   "totp_generation"};
      const std::set<std::string> session_keys{"id",
                                               "user_id",
                                               "mfa_complete",
                                               "created_at",
                                               "last_seen_at",
                                               "revoked_at",
                                               "totp_verified_generation",
                                               "totp_verified_at"};
      for (const auto& event : events) {
        const auto serialized = event.before_json + event.after_json;
        EXPECT_EQ(serialized.find(k_totp_secret), std::string::npos);
        EXPECT_EQ(serialized.find(first.token_hash), std::string::npos);
        EXPECT_EQ(serialized.find(first.token_prefix), std::string::npos);
        EXPECT_EQ(serialized.find("auth_bindings"), std::string::npos);
        if (event.request_id != ctx.request_id) {
          continue;
        }
        actions.insert(event.action);
        EXPECT_EQ(event.actor_user_id, ctx.actor_user_id);
        EXPECT_EQ(event.actor_session_id, ctx.actor_session_id);
        EXPECT_FALSE(event.lab_id);
        const auto before = nlohmann::json::parse(event.before_json);
        const auto after = nlohmann::json::parse(event.after_json);
        if (event.action == "auth.totp_secret_changed") {
          EXPECT_EQ(before.size(), secret_keys.size());
          EXPECT_EQ(after.size(), secret_keys.size());
          EXPECT_EQ(before.at("enrolled"), !before.at("generation").is_null());
          EXPECT_EQ(after.at("enrolled"), true);
          EXPECT_FALSE(after.at("generation").is_null());
          const auto expected_generation = before.at("generation").is_null()
                                               ? core::json_helpers::opt_to_json(first_generation)
                                               : core::json_helpers::opt_to_json(last_generation);
          EXPECT_EQ(after.at("generation"), expected_generation);
          if (!before.at("generation").is_null()) {
            EXPECT_EQ(before.at("generation"), core::json_helpers::opt_to_json(first_generation));
          }
        } else if (event.action == "auth.session_revoked") {
          EXPECT_EQ(before.size(), session_keys.size());
          EXPECT_EQ(after.size(), session_keys.size());
          EXPECT_EQ(before.at("user_id"), user1_.to_string());
          EXPECT_EQ(after.at("user_id"), user1_.to_string());
          EXPECT_EQ(before.at("id"), after.at("id"));
          EXPECT_TRUE(before.at("id") == first.id.to_string() ||
                      before.at("id") == second.id.to_string());
          EXPECT_TRUE(before.at("revoked_at").is_null());
          EXPECT_FALSE(after.at("revoked_at").is_null());
          EXPECT_EQ(before.at("mfa_complete"), after.at("mfa_complete"));
          EXPECT_EQ(before.at("last_seen_at"), after.at("last_seen_at"));
          EXPECT_EQ(before.at("totp_verified_generation"), after.at("totp_verified_generation"));
        } else {
          EXPECT_EQ(after.size(), state_keys.size());
          EXPECT_EQ(after.at("user_id"), user1_.to_string());
          EXPECT_EQ(after.at("failure_count"), 0);
          EXPECT_TRUE(after.at("last_accepted_step").is_null());
          if (event.action == "auth.mfa_state_initialized") {
            EXPECT_TRUE(before.empty());
            EXPECT_TRUE(after.at("totp_generation").is_null());
          } else {
            EXPECT_EQ(before.size(), state_keys.size());
            EXPECT_EQ(before.at("user_id"), user1_.to_string());
            EXPECT_EQ(before.at("failure_count"), after.at("failure_count"));
            EXPECT_EQ(before.at("last_accepted_step"), after.at("last_accepted_step"));
            EXPECT_EQ(before.at("observed_at"), after.at("observed_at"));
            EXPECT_EQ(before.at("next_allowed_at"), after.at("next_allowed_at"));
            EXPECT_NE(before.at("totp_generation"), after.at("totp_generation"));
            const auto expected_generation = before.at("totp_generation").is_null()
                                                 ? core::json_helpers::opt_to_json(first_generation)
                                                 : core::json_helpers::opt_to_json(last_generation);
            EXPECT_EQ(after.at("totp_generation"), expected_generation);
            if (!before.at("totp_generation").is_null()) {
              EXPECT_EQ(before.at("totp_generation"),
                        core::json_helpers::opt_to_json(first_generation));
            }
          }
        }
        for (const auto& image : {event.before_json, event.after_json}) {
          const auto snapshot = nlohmann::json::parse(image);
          const auto& allowed = event.action == "auth.totp_secret_changed" ? secret_keys
                                : event.entity_kind == "session"           ? session_keys
                                                                           : state_keys;
          for (const auto& [key, value] : snapshot.items()) {
            (void)value;
            EXPECT_TRUE(allowed.contains(key)) << key;
          }
        }
      }
      EXPECT_EQ(actions,
                (std::multiset<std::string>{
                    "auth.mfa_state_initialized", "auth.mfa_generation_changed",
                    "auth.mfa_generation_changed", "auth.totp_secret_changed",
                    "auth.totp_secret_changed", "auth.session_revoked", "auth.session_revoked"}));
      EXPECT_TRUE(audit::verify_audit_chain(events).ok);
      auto read = backend().begin(IsolationLevel::ReadCommitted);
      EXPECT_EQ(read->repo<core::MfaSecurityState>().find_by_id(user1_)->totp_generation,
                last_generation);
      EXPECT_TRUE(read->repo<core::Session>().find_by_id(first.id)->revoked_at);
      EXPECT_TRUE(read->repo<core::Session>().find_by_id(second.id)->revoked_at);
      read->commit();
    }

    TEST_P(SessionRepositoryTest,
           DelayedOrdinaryProfileFlushCannotRestoreSecretAfterLifecycleCommit) {
      auto profile = backend().begin(IsolationLevel::ReadCommitted);
      auto stale = profile->repo<core::User>().find_by_id(user1_).value();
      stale.display_name = "delayed profile";
      if (GetParam() == BackendKind::Sqlite) {
        profile->repo<core::User>().update(stale, mutation_context());
      }
      auto enroll = backend().begin_auth_transaction({user1_});
      enroll->replace_totp_secret(user1_, std::string(k_totp_secret), mutation_context());
      enroll->commit();
      if (GetParam() == BackendKind::Sqlite) {
        profile->commit();
      } else {
        EXPECT_THROW(profile->repo<core::User>().update(stale, mutation_context()),
                     ConstraintViolation);
        profile->rollback();
      }
      auto read = backend().begin(IsolationLevel::ReadCommitted);
      const auto user = read->repo<core::User>().find_by_id(user1_).value();
      EXPECT_EQ(user.totp_secret_enc, k_totp_secret);
      if (GetParam() == BackendKind::Sqlite) {
        EXPECT_EQ(user.display_name, stale.display_name);
      }
      read->commit();
      EXPECT_TRUE(audit::verify_audit_chain(audit_events()).ok);
    }

    TEST_P(SessionRepositoryTest, TotpRemovalReenrollmentAndFailurePreserveExistingStateAndProof) {
      core::MfaSecurityState before;
      core::Session proven;
      {
        auto txn = backend().begin_auth_transaction({user1_});
        txn->replace_totp_secret(user1_, std::string(k_totp_secret), mutation_context());
        before = txn->repo<core::MfaSecurityState>().find_by_id(user1_).value();
        before.last_accepted_step = 100;
        before.failure_count = 17;
        before.observed_at = ts(1000);
        before.next_allowed_at = ts(86'400'001'000LL);
        txn->repo<core::MfaSecurityState>().update(before, mutation_context());
        auto pending = make_session(99, user1_);
        pending.mfa_complete = false;
        txn->repo<core::Session>().insert(pending, mutation_context());
        txn->complete_totp_session(pending.id, before.totp_generation.value(), ts(5000),
                                   mutation_context());
        proven = txn->repo<core::Session>().find_by_id(pending.id).value();
        txn->commit();
      }
      const auto count = audit_event_count();
      {
        auto txn = backend().begin_auth_transaction({user1_});
        txn->replace_totp_secret(user1_, std::nullopt, mutation_context());
        fail_next_audit_append();
        EXPECT_THROW(txn->commit(), BackendError);
      }
      {
        auto read = backend().begin(IsolationLevel::ReadCommitted);
        EXPECT_EQ(read->repo<core::MfaSecurityState>().find_by_id(user1_), before);
        EXPECT_EQ(read->repo<core::Session>().find_by_id(proven.id), proven);
        EXPECT_EQ(read->repo<core::User>().find_by_id(user1_)->totp_secret_enc, k_totp_secret);
        read->commit();
      }
      EXPECT_EQ(audit_event_count(), count);
      auto txn = backend().begin_auth_transaction({user1_});
      txn->replace_totp_secret(user1_, std::nullopt, mutation_context());
      EXPECT_FALSE(txn->repo<core::MfaSecurityState>().find_by_id(user1_)->totp_generation);
      txn->replace_totp_secret(user1_, std::string(k_totp_secret), mutation_context());
      auto after = txn->repo<core::MfaSecurityState>().find_by_id(user1_).value();
      EXPECT_NE(after.totp_generation, before.totp_generation);
      after.totp_generation = before.totp_generation;
      EXPECT_EQ(after, before);
      txn->commit();
      auto read = backend().begin(IsolationLevel::ReadCommitted);
      EXPECT_TRUE(read->repo<core::Session>().find_by_id(proven.id)->revoked_at);
      EXPECT_EQ(read->repo<core::Session>().find_by_id(proven.id)->totp_verified_generation,
                proven.totp_verified_generation);
      read->commit();
      EXPECT_TRUE(audit::verify_audit_chain(audit_events()).ok);
    }

    TEST_P(SessionRepositoryTest,
           TotpProofCompletionFailureAndGenericWritesCannotElevateOrEraseProof) {
      core::Uuid generation;
      auto pending = make_session(88, user1_);
      pending.mfa_complete = false;
      {
        auto txn = backend().begin_auth_transaction({user1_});
        txn->replace_totp_secret(user1_, std::string(k_totp_secret), mutation_context());
        generation =
            txn->repo<core::MfaSecurityState>().find_by_id(user1_)->totp_generation.value();
        txn->repo<core::Session>().insert(pending, mutation_context());
        txn->repo<core::Session>().insert(make_session(89, user1_), mutation_context());
        auto half = make_session(90, user1_);
        half.totp_verified_at = ts(5000);
        EXPECT_THROW(txn->repo<core::Session>().insert(half, mutation_context()),
                     ConstraintViolation);
        half.totp_verified_at.reset();
        half.totp_verified_generation = generation;
        EXPECT_THROW(txn->repo<core::Session>().insert(half, mutation_context()),
                     ConstraintViolation);
        txn->commit();
      }
      {
        auto txn = backend().begin_auth_transaction({user2_});
        EXPECT_THROW(
            txn->complete_totp_session(pending.id, generation, ts(5000), mutation_context()),
            BackendError);
      }
      const auto count = audit_event_count();
      {
        auto txn = backend().begin_auth_transaction({user1_});
        EXPECT_THROW(txn->complete_totp_session(id_from_low<core::SessionId>(89), generation,
                                                ts(5000), mutation_context()),
                     ConstraintViolation);
        txn->complete_totp_session(pending.id, generation, ts(5000), mutation_context());
        fail_next_audit_append();
        EXPECT_THROW(txn->commit(), BackendError);
      }
      EXPECT_EQ(audit_event_count(), count);
      EXPECT_EQ(read_session(pending.id), pending);
      auto txn = backend().begin_auth_transaction({user1_});
      txn->complete_totp_session(pending.id, generation, ts(5000), mutation_context());
      auto completed = txn->repo<core::Session>().find_by_id(pending.id).value();
      auto clear = completed;
      clear.totp_verified_generation.reset();
      clear.totp_verified_at.reset();
      EXPECT_THROW(txn->repo<core::Session>().update(clear, mutation_context()),
                   ConstraintViolation);
      clear = completed;
      clear.totp_verified_generation = core::Uuid{};
      EXPECT_THROW(txn->repo<core::Session>().update(clear, mutation_context()),
                   ConstraintViolation);
      txn->commit();
      EXPECT_EQ(read_session(pending.id), completed);
    }

    TEST_P(SessionRepositoryTest,
           MfaDatabaseConstraintsRejectInvalidStateAndUnpairedOrMalformedProof) {
      {
        auto txn = backend().begin_auth_transaction({user1_});
        (void)txn->initialize_mfa_state(user1_, mutation_context());
        txn->repo<core::Session>().insert(make_session(77, user1_), mutation_context());
        txn->commit();
      }
      const auto user = user1_.to_string();
      const auto session = id_from_low<core::SessionId>(77).to_string();
      const std::vector<std::string> invalid{
          "UPDATE mfa_security_states SET failure_count = 18 WHERE user_id = '" + user + "'",
          "UPDATE mfa_security_states SET failure_count = -1 WHERE user_id = '" + user + "'",
          "UPDATE mfa_security_states SET last_accepted_step = -1 WHERE user_id = '" + user + "'",
          "UPDATE mfa_security_states SET user_id = NULL WHERE user_id = '" + user + "'",
          "UPDATE mfa_security_states SET totp_generation = '------------------------------------' "
          "WHERE user_id = '" +
              user + "'",
          "UPDATE mfa_security_states SET user_id = '00000000-0000-0000-0000-000000009999' WHERE "
          "user_id = '" +
              user + "'",
          "UPDATE sessions SET totp_verified_generation = '00000000-0000-0000-0000-000000000001' "
          "WHERE id = '" +
              session + "'",
          "UPDATE sessions SET totp_verified_at_micros = 5000 WHERE id = '" + session + "'",
          "UPDATE sessions SET totp_verified_generation = '------------------------------------', "
          "totp_verified_at_micros = 5000 WHERE id = '" +
              session + "'",
          "UPDATE sessions SET mfa_complete = " +
              std::string(GetParam() == BackendKind::Sqlite ? "0" : "false") +
              ", totp_verified_generation = '00000000-0000-0000-0000-000000000001', "
              "totp_verified_at_micros = 5000 WHERE id = '" +
              session + "'"};
      for (const auto& sql : invalid) {
        auto txn = backend().begin_auth_transaction({user1_});
        EXPECT_THROW(mfa_fixture_sql(*txn, sql), BackendError);
      }
    }

    TEST_P(SessionRepositoryTest,
           MigrationSeventeenLeavesLegacyProofNullAndStateEmptyUntilAuditedInitialization) {
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Session>().insert(make_session(66, user1_), mutation_context());
        txn->commit();
      }
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        mfa_fixture_sql(*txn, "DROP TABLE mfa_security_states");
        if (GetParam() == BackendKind::Postgres) {
          mfa_fixture_sql(*txn, "ALTER TABLE sessions DROP CONSTRAINT sessions_totp_proof_pair");
        }
        mfa_fixture_sql(*txn, "ALTER TABLE sessions DROP COLUMN totp_verified_at_micros");
        mfa_fixture_sql(*txn, "ALTER TABLE sessions DROP COLUMN totp_verified_generation");
        mfa_fixture_sql(*txn,
                        "DELETE FROM role_permissions WHERE permission_key = 'auth.mfa_unlock'");
        mfa_fixture_sql(*txn, "DELETE FROM permissions WHERE key = 'auth.mfa_unlock'");
        mfa_fixture_sql(*txn, "DELETE FROM schema_migrations WHERE version = 17");
        // Explicit legacy-16 fixture: that schema had no guarded secret lifecycle.
        mfa_fixture_sql(*txn, "UPDATE users SET totp_secret_enc = '" + std::string(k_totp_secret) +
                                  "' WHERE id = '" + user1_.to_string() + "'");
        txn->commit();
      }
      ASSERT_EQ(backend().current_version(), SchemaVersion{16});
      const auto count = audit_event_count();
      ASSERT_NO_THROW(backend().migrate_to_latest());
      EXPECT_EQ(backend().current_version(), SchemaVersion{17});
      auto read = backend().begin(IsolationLevel::ReadCommitted);
      EXPECT_FALSE(read->repo<core::MfaSecurityState>().find_by_id(user1_));
      const auto legacy =
          read->repo<core::Session>().find_by_id(id_from_low<core::SessionId>(66)).value();
      EXPECT_TRUE(legacy.mfa_complete);
      EXPECT_FALSE(legacy.totp_verified_generation);
      EXPECT_FALSE(legacy.totp_verified_at);
      read->commit();
      EXPECT_EQ(audit_event_count(), count);
      auto txn = backend().begin_auth_transaction({user1_});
      const auto state = txn->initialize_mfa_state(user1_, mutation_context());
      EXPECT_TRUE(state.totp_generation);
      EXPECT_FALSE(state.last_accepted_step);
      EXPECT_EQ(state.failure_count, 0);
      txn->commit();
      EXPECT_EQ(audit_event_count(), count + 1);
    }

    INSTANTIATE_TEST_SUITE_P(Backends, SessionRepositoryTest,
                             ::testing::Values(BackendKind::Sqlite, BackendKind::Postgres),
                             [](const ::testing::TestParamInfo<BackendKind>& info) {
                               return backend_kind_name(info.param);
                             });

  } // namespace
} // namespace fmgr::storage
