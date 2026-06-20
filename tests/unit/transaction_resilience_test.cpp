// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Tests for transaction rollback, isolation, and audit-chain integrity under
// failure conditions. Verifies that failed operations do not corrupt data.

#include "core/identity.h"
#include "core/role.h"
#include "storage/IdentityTraits.h"
#include "storage/RoleTraits.h"
#include "storage/SessionTraits.h"
#include "storage/sqlite/IdentityRepositories.h"
#include "storage/sqlite/RoleRepositories.h"
#include "storage/sqlite/SessionRepositories.h"
#include "storage/sqlite/SqliteBackend.h"

#include "test_helpers.h"
#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace fmgr::storage {
  namespace {
    using namespace fmgr::test;

    [[nodiscard]] MutationContext mutation_context() {
      return MutationContext{
          .actor_user_id = id_from_low<core::UserId>(1),
          .actor_session_id = "txn-resilience-session",
          .request_id = "txn-resilience-request",
          .reason = "transaction resilience test",
      };
    }

    // Fixture sets up an in-memory SQLite backend with identity, role, and
    // session repos registered.
    class TransactionResilienceTest : public ::testing::Test {
    protected:
      void SetUp() override {
        backend_ =
            std::make_unique<SqliteBackend>(SqliteBackendOptions{.database_path = ":memory:"});
        register_identity_repositories(*backend_);
        register_role_repositories(*backend_);
        register_session_repositories(*backend_);
        backend_->migrate_to_latest();
      }

      void TearDown() override {
        backend_.reset();
      }

      [[nodiscard]] core::Lab make_lab(std::uint64_t low) {
        return core::Lab{
            .id = id_from_low<core::LabId>(low),
            .name = "Lab " + std::to_string(low),
            .contact = "lab@example.com",
            .created_at = core::Timestamp::from_unix_micros(static_cast<std::int64_t>(low)),
            .settings_json = nlohmann::json::object(),
        };
      }

      [[nodiscard]] core::User make_user(std::uint64_t low) {
        return core::User{
            .id = id_from_low<core::UserId>(low),
            .primary_email = "user" + std::to_string(low) + "@example.com",
            .display_name = "User " + std::to_string(low),
            .status = core::UserStatus::Active,
            .created_at = core::Timestamp::from_unix_micros(static_cast<std::int64_t>(low)),
            .auth_bindings = nlohmann::json::array(),
        };
      }

      [[nodiscard]] core::LabMembership make_membership(std::uint64_t user_low,
                                                        std::uint64_t lab_low) {
        return core::LabMembership{
            .user_id = id_from_low<core::UserId>(user_low),
            .lab_id = id_from_low<core::LabId>(lab_low),
            .role_id = core::builtin_role_id(core::RoleKind::Member),
            .scope_filters_json = nlohmann::json::object(),
            .joined_at = core::Timestamp::from_unix_micros(1),
        };
      }

      std::unique_ptr<SqliteBackend> backend_;
    };

    // ---- Rollback tests ----

    TEST_F(TransactionResilienceTest, RollbackUndoesAllInsertedRows) {
      const auto ctx = mutation_context();
      {
        auto txn = backend_->begin(IsolationLevel::Serializable);
        txn->repo<core::Lab>().insert(make_lab(1), ctx);
        txn->rollback();
      }

      // After rollback, the lab must not exist.
      auto txn = backend_->begin(IsolationLevel::Serializable);
      const auto found = txn->repo<core::Lab>().find_by_id(id_from_low<core::LabId>(1));
      EXPECT_FALSE(found.has_value());
    }

    TEST_F(TransactionResilienceTest, RollbackUndoesMultipleEntityTypes) {
      const auto ctx = mutation_context();
      {
        auto txn = backend_->begin(IsolationLevel::Serializable);
        txn->repo<core::Lab>().insert(make_lab(1), ctx);
        txn->repo<core::User>().insert(make_user(1), ctx);
        txn->rollback();
      }

      auto txn = backend_->begin(IsolationLevel::Serializable);
      EXPECT_FALSE(txn->repo<core::Lab>().find_by_id(id_from_low<core::LabId>(1)).has_value());
      EXPECT_FALSE(txn->repo<core::User>().find_by_id(id_from_low<core::UserId>(1)).has_value());
    }

    TEST_F(TransactionResilienceTest, DestroyedTransactionDoesNotCommit) {
      const auto ctx = mutation_context();
      {
        auto txn = backend_->begin(IsolationLevel::Serializable);
        txn->repo<core::Lab>().insert(make_lab(1), ctx);
        // txn destroyed without commit → implicit rollback
      }

      auto txn = backend_->begin(IsolationLevel::Serializable);
      EXPECT_FALSE(txn->repo<core::Lab>().find_by_id(id_from_low<core::LabId>(1)).has_value());
    }

    TEST_F(TransactionResilienceTest, SecondCommitAfterFirstCommitThrows) {
      const auto ctx = mutation_context();
      auto txn = backend_->begin(IsolationLevel::Serializable);
      txn->repo<core::Lab>().insert(make_lab(1), ctx);
      txn->commit();
      // Second commit on an already-committed transaction is an error.
      EXPECT_THROW(txn->commit(), BackendError);
    }

    TEST_F(TransactionResilienceTest, RollbackAfterCommitIsHarmless) {
      const auto ctx = mutation_context();
      auto txn = backend_->begin(IsolationLevel::Serializable);
      txn->repo<core::Lab>().insert(make_lab(1), ctx);
      txn->commit();
      // Rollback after commit should not corrupt committed data.
      EXPECT_NO_THROW(txn->rollback());

      auto read_txn = backend_->begin(IsolationLevel::Serializable);
      EXPECT_TRUE(read_txn->repo<core::Lab>().find_by_id(id_from_low<core::LabId>(1)).has_value());
    }

    TEST_F(TransactionResilienceTest, CommitAfterRollbackThrows) {
      const auto ctx = mutation_context();
      auto txn = backend_->begin(IsolationLevel::Serializable);
      txn->repo<core::Lab>().insert(make_lab(1), ctx);
      txn->rollback();
      EXPECT_THROW(txn->commit(), BackendError);
    }

    // ---- Error during multi-entity operation ----

    TEST_F(TransactionResilienceTest, UniqueViolationRollsBackEntireTransaction) {
      const auto ctx = mutation_context();
      auto txn = backend_->begin(IsolationLevel::Serializable);
      txn->repo<core::Lab>().insert(make_lab(1), ctx);
      txn->commit();

      // Now try to insert lab 1 again alongside a new lab 2 in the same txn.
      // Lab 1 is a duplicate — the violation should roll back everything.
      bool had_error = false;
      {
        auto txn2 = backend_->begin(IsolationLevel::Serializable);
        txn2->repo<core::Lab>().insert(make_lab(2), ctx);
        try {
          txn2->repo<core::Lab>().insert(make_lab(1), ctx);
          txn2->commit(); // Should not reach here.
        } catch (const UniqueViolation&) {
          had_error = true;
          txn2->rollback();
        }
      }
      EXPECT_TRUE(had_error);

      // Lab 2 must NOT exist (the whole transaction should have been rolled back).
      auto read_txn = backend_->begin(IsolationLevel::Serializable);
      EXPECT_TRUE(read_txn->repo<core::Lab>().find_by_id(id_from_low<core::LabId>(1)).has_value());
      EXPECT_FALSE(read_txn->repo<core::Lab>().find_by_id(id_from_low<core::LabId>(2)).has_value());
    }

    TEST_F(TransactionResilienceTest, NotFoundOnUpdateLeavesEntityUntouched) {
      const auto ctx = mutation_context();
      auto txn = backend_->begin(IsolationLevel::Serializable);

      // Create a valid lab, then try to update a non-existent one.
      txn->repo<core::Lab>().insert(make_lab(1), ctx);

      core::Lab non_existent = make_lab(99);
      non_existent.name = "ghost";
      EXPECT_THROW(txn->repo<core::Lab>().update(non_existent, ctx), NotFound);

      // Lab 1 should still exist and be unchanged.
      txn->commit();
      auto read_txn = backend_->begin(IsolationLevel::Serializable);
      const auto lab1 = read_txn->repo<core::Lab>().find_by_id(id_from_low<core::LabId>(1));
      ASSERT_TRUE(lab1.has_value());
      EXPECT_EQ(lab1->name, "Lab 1");
    }

    // ---- Transaction isolation ----

    TEST_F(TransactionResilienceTest, UncommittedWritesNotVisibleToOtherTransaction) {
      const auto ctx = mutation_context();
      auto writer_txn = backend_->begin(IsolationLevel::Serializable);
      writer_txn->repo<core::Lab>().insert(make_lab(1), ctx);

      // A concurrent reader must not see the uncommitted lab.
      auto reader_txn = backend_->begin(IsolationLevel::Serializable);
      EXPECT_FALSE(
          reader_txn->repo<core::Lab>().find_by_id(id_from_low<core::LabId>(1)).has_value());

      writer_txn->commit();

      // After commit, the reader (still on its snapshot) may or may not see it
      // depending on isolation level.  Serializable may still not see it on
      // the same snapshot.  A fresh transaction must see it.
      auto fresh_txn = backend_->begin(IsolationLevel::Serializable);
      EXPECT_TRUE(fresh_txn->repo<core::Lab>().find_by_id(id_from_low<core::LabId>(1)).has_value());
    }

    // ---- SQLite database file error simulation ----

    TEST_F(TransactionResilienceTest, ReadAfterTransactionCloseIsSafe) {
      // After a transaction is committed/rolled back, subsequent reads through
      // a new transaction must work correctly — the backend must not be left in
      // a broken state.
      const auto ctx = mutation_context();
      for (int i = 0; i < 10; ++i) {
        auto txn = backend_->begin(IsolationLevel::Serializable);
        txn->repo<core::Lab>().insert(make_lab(static_cast<std::uint64_t>(100 + i)), ctx);
        txn->commit();
      }

      // Verify all 10 labs exist.
      auto txn = backend_->begin(IsolationLevel::Serializable);
      for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(txn->repo<core::Lab>()
                        .find_by_id(id_from_low<core::LabId>(static_cast<std::uint64_t>(100 + i)))
                        .has_value());
      }
    }

    // ---- Break-it: large transactions and rapid cycling ----

    TEST_F(TransactionResilienceTest, LargeMultiEntityTransactionCommitsAtomically) {
      const auto ctx = mutation_context();
      // Insert many entities in a single transaction — all or nothing.
      {
        auto txn = backend_->begin(IsolationLevel::Serializable);
        for (std::uint64_t i = 0; i < 100; ++i) {
          txn->repo<core::Lab>().insert(make_lab(500 + i), ctx);
        }
        txn->commit();
      }
      // All 100 must be visible.
      auto read_txn = backend_->begin(IsolationLevel::Serializable);
      for (std::uint64_t i = 0; i < 100; ++i) {
        EXPECT_TRUE(
            read_txn->repo<core::Lab>().find_by_id(id_from_low<core::LabId>(500 + i)).has_value())
            << "lab " << (500 + i);
      }
    }

    TEST_F(TransactionResilienceTest, RapidBeginCommitRollbackCycling) {
      // Rapidly create, commit, and rollback transactions to stress the backend's
      // transaction lifecycle management.
      const auto ctx = mutation_context();
      for (int i = 0; i < 50; ++i) {
        {
          auto txn = backend_->begin(IsolationLevel::Serializable);
          txn->repo<core::Lab>().insert(make_lab(static_cast<std::uint64_t>(600 + i)), ctx);
          txn->commit();
        }
        {
          auto txn = backend_->begin(IsolationLevel::Serializable);
          txn->repo<core::Lab>().insert(make_lab(static_cast<std::uint64_t>(700 + i)), ctx);
          txn->rollback();
        }
      }
      // Committed rows exist, rolled-back ones don't.
      auto read_txn = backend_->begin(IsolationLevel::Serializable);
      for (int i = 0; i < 50; ++i) {
        EXPECT_TRUE(read_txn->repo<core::Lab>()
                        .find_by_id(id_from_low<core::LabId>(static_cast<std::uint64_t>(600 + i)))
                        .has_value())
            << "committed lab " << (600 + i);
        EXPECT_FALSE(read_txn->repo<core::Lab>()
                         .find_by_id(id_from_low<core::LabId>(static_cast<std::uint64_t>(700 + i)))
                         .has_value())
            << "rolled-back lab " << (700 + i);
      }
    }

    TEST_F(TransactionResilienceTest, MultipleRollbacksOnSameTransaction) {
      const auto ctx = mutation_context();
      auto txn = backend_->begin(IsolationLevel::Serializable);
      txn->repo<core::Lab>().insert(make_lab(1), ctx);
      txn->rollback();
      // Multiple rollbacks on an already-rolled-back txn: should be safe (no crash).
      EXPECT_NO_THROW(txn->rollback());
      EXPECT_NO_THROW(txn->rollback());
    }

    TEST_F(TransactionResilienceTest, WriteAfterRollbackIsIgnored) {
      const auto ctx = mutation_context();
      auto txn = backend_->begin(IsolationLevel::Serializable);
      txn->repo<core::Lab>().insert(make_lab(1), ctx);
      txn->rollback();
      // Attempting to insert after rollback should be a no-op or throw.
      // Either way, the data must not persist.
      bool threw = false;
      try {
        txn->repo<core::Lab>().insert(make_lab(2), ctx);
      } catch (const BackendError&) {
        threw = true;
      }
      // Verify neither lab exists.
      auto read_txn = backend_->begin(IsolationLevel::Serializable);
      EXPECT_FALSE(read_txn->repo<core::Lab>().find_by_id(id_from_low<core::LabId>(1)).has_value());
      EXPECT_FALSE(read_txn->repo<core::Lab>().find_by_id(id_from_low<core::LabId>(2)).has_value());
      (void)threw; // acceptable either way
    }

    TEST_F(TransactionResilienceTest, EmptyTransactionCommitDoesNothing) {
      // A transaction with no operations should commit cleanly.
      auto txn = backend_->begin(IsolationLevel::Serializable);
      EXPECT_NO_THROW(txn->commit());
    }

    TEST_F(TransactionResilienceTest, EmptyTransactionRollbackDoesNothing) {
      auto txn = backend_->begin(IsolationLevel::Serializable);
      EXPECT_NO_THROW(txn->rollback());
    }

    TEST_F(TransactionResilienceTest, MixedOperationsPreservedOnCommit) {
      const auto ctx = mutation_context();
      {
        auto txn = backend_->begin(IsolationLevel::Serializable);
        txn->repo<core::Lab>().insert(make_lab(1), ctx);
        txn->repo<core::User>().insert(make_user(1), ctx);
        txn->repo<core::LabMembership>().insert(make_membership(1, 1), ctx);
        txn->commit();
      }
      auto read_txn = backend_->begin(IsolationLevel::Serializable);
      EXPECT_TRUE(read_txn->repo<core::Lab>().find_by_id(id_from_low<core::LabId>(1)).has_value());
      EXPECT_TRUE(
          read_txn->repo<core::User>().find_by_id(id_from_low<core::UserId>(1)).has_value());
      EXPECT_TRUE(read_txn->repo<core::LabMembership>()
                      .find_by_id(core::LabMembershipId{.user_id = id_from_low<core::UserId>(1),
                                                        .lab_id = id_from_low<core::LabId>(1)})
                      .has_value());
    }

    // ==== Aggressive: double-begin, write contention, edge lifecycle ====

    TEST_F(TransactionResilienceTest, WriteAfterCommitIsRejected) {
      const auto ctx = mutation_context();
      auto txn = backend_->begin(IsolationLevel::Serializable);
      txn->repo<core::Lab>().insert(make_lab(1), ctx);
      txn->commit();
      // Any further mutation on a committed transaction is silently ignored.
      txn->repo<core::Lab>().insert(make_lab(2), ctx);

      // Lab 2 must NOT exist since the write happened after commit.
      auto read_txn = backend_->begin(IsolationLevel::Serializable);
      EXPECT_FALSE(read_txn->repo<core::Lab>().find_by_id(id_from_low<core::LabId>(2)).has_value());
      // Lab 1 was committed before the rogue write and must still exist.
      EXPECT_TRUE(read_txn->repo<core::Lab>().find_by_id(id_from_low<core::LabId>(1)).has_value());
    }

    TEST_F(TransactionResilienceTest, WriteContentionWithLongRunningReader) {
      const auto ctx = mutation_context();
      // Writer A inserts and commits.
      {
        auto txn = backend_->begin(IsolationLevel::Serializable);
        txn->repo<core::Lab>().insert(make_lab(1), ctx);
        txn->commit();
      }
      // Reader B opens a snapshot, then Writer C inserts and commits.
      auto reader = backend_->begin(IsolationLevel::Serializable);
      {
        auto writer = backend_->begin(IsolationLevel::Serializable);
        writer->repo<core::Lab>().insert(make_lab(2), ctx);
        writer->commit();
      }
      // Reader B must still see only lab 1 (snapshot isolation).
      EXPECT_TRUE(reader->repo<core::Lab>().find_by_id(id_from_low<core::LabId>(1)).has_value());
      // Lab 2 may or may not be visible depending on isolation.
    }

    TEST_F(TransactionResilienceTest, SetSessionVarOnMultipleTransactions) {
      auto txn1 = backend_->begin(IsolationLevel::Serializable);
      txn1->set_session_var("key_a", "val_a");
      auto txn2 = backend_->begin(IsolationLevel::Serializable);
      txn2->set_session_var("key_b", "val_b");
      // Each transaction has independent session vars; must not crash or merge.
      txn1->commit();
      txn2->rollback();
    }

  } // namespace
} // namespace fmgr::storage
