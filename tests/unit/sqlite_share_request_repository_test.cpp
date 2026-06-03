// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/identity.h"
#include "core/share_request.h"
#include "storage/IdentityTraits.h"
#include "storage/ShareRequestTraits.h"
#include "storage/sqlite/IdentityRepositories.h"
#include "storage/sqlite/ShareRequestRepositories.h"
#include "storage/sqlite/SqliteBackend.h"

#include "test_helpers.h"
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace fmgr::storage {
  namespace {
    using namespace fmgr::test;




    [[nodiscard]] std::filesystem::path sqlite_test_path(std::string_view suffix) {
      const auto unique = std::to_string(static_cast<unsigned long long>(
                              ::testing::UnitTest::GetInstance()->random_seed())) +
                          "-" + std::to_string(reinterpret_cast<std::uintptr_t>(&suffix));
      return std::filesystem::temp_directory_path() / (std::string("freezermanager-sqlite-sr-") +
                                                       unique + "-" + std::string(suffix) + ".db");
    }


    [[nodiscard]] MutationContext mutation_context() {
      return MutationContext{
          .actor_user_id = id_from_low<core::UserId>(999),
          .actor_session_id = "sr-test-session",
          .request_id = "sr-test-request",
          .reason = "share_request repository test",
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

    [[nodiscard]] core::ShareRequest make_share_request(std::uint64_t low_bits,
                                                        core::LabId source_lab_id,
                                                        core::LabId target_lab_id,
                                                        core::UserId requested_by) {
      return core::ShareRequest{
          .id = id_from_low<core::ShareRequestId>(low_bits),
          .source_lab_id = source_lab_id,
          .target_lab_id = target_lab_id,
          .requested_by = requested_by,
          .scope_json = "{}",
          .status = core::ShareRequestStatus::Pending,
          .created_at = ts(300 + static_cast<std::int64_t>(low_bits)),
          .decided_at = std::nullopt,
      };
    }

    [[nodiscard]] core::ShareRequestApproval
    make_approval(core::ShareRequestId sr_id, core::ShareApprovalRole role, core::UserId user_id) {
      return core::ShareRequestApproval{
          .share_request_id = sr_id,
          .approver_role = role,
          .approver_user_id = user_id,
          .decided_at = ts(500000),
          .note = std::nullopt,
      };
    }

    class SqliteShareRequestRepositoryTest : public ::testing::Test {
    protected:
      SqliteShareRequestRepositoryTest()
          : db_path_(sqlite_test_path("sr-repo")),
            backend_(SqliteBackendOptions{.database_path = db_path_.string()}) {
        register_identity_repositories(backend_);
        register_share_request_repositories(backend_);
      }

      void SetUp() override {
        remove_sqlite_files(db_path_);
        backend_.migrate_to_latest();

        // Seed two labs and one user each in separate committed transactions.
        // Two separate labs are needed to satisfy the source != target CHECK.
        {
          auto txn = backend().begin(IsolationLevel::Serializable);
          txn->repo<core::Lab>().insert(make_lab(1), mutation_context());
          txn->commit();
        }
        {
          auto txn = backend().begin(IsolationLevel::Serializable);
          txn->repo<core::Lab>().insert(make_lab(2), mutation_context());
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
          txn->repo<core::User>().insert(make_user(20, id_from_low<core::LabId>(2)),
                                         mutation_context());
          txn->commit();
        }

        lab1_ = id_from_low<core::LabId>(1);
        lab2_ = id_from_low<core::LabId>(2);
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
      core::LabId lab2_;
      core::UserId user1_;
      core::UserId user2_;
    };

    // ---- ShareRequest tests ----

    TEST_F(SqliteShareRequestRepositoryTest, InsertAndFindById) {
      const auto sr = make_share_request(1, lab1_, lab2_, user1_);
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::ShareRequest>().insert(sr, mutation_context());
        txn->commit();
      }
      auto txn = backend().begin(IsolationLevel::Serializable);
      const auto found = txn->repo<core::ShareRequest>().find_by_id(sr.id);
      ASSERT_TRUE(found.has_value());
      EXPECT_EQ(found->id, sr.id);
      EXPECT_EQ(found->source_lab_id, lab1_);
      EXPECT_EQ(found->target_lab_id, lab2_);
      EXPECT_EQ(found->status, core::ShareRequestStatus::Pending);
    }

    TEST_F(SqliteShareRequestRepositoryTest, DuplicateIdThrowsUniqueViolation) {
      const auto sr = make_share_request(1, lab1_, lab2_, user1_);
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::ShareRequest>().insert(sr, mutation_context());
        txn->commit();
      }
      auto txn = backend().begin(IsolationLevel::Serializable);
      EXPECT_THROW(txn->repo<core::ShareRequest>().insert(sr, mutation_context()), UniqueViolation);
    }

    TEST_F(SqliteShareRequestRepositoryTest, SameSourceAndTargetLabThrowsConstraintViolation) {
      auto sr = make_share_request(1, lab1_, lab2_, user1_);
      sr.target_lab_id = lab1_; // same as source
      auto txn = backend().begin(IsolationLevel::Serializable);
      EXPECT_THROW(txn->repo<core::ShareRequest>().insert(sr, mutation_context()),
                   ConstraintViolation);
    }

    TEST_F(SqliteShareRequestRepositoryTest, SoftDeleteSetsStatusRevokedAndDefaultQueryExcludesIt) {
      const auto sr = make_share_request(1, lab1_, lab2_, user1_);
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::ShareRequest>().insert(sr, mutation_context());
        txn->commit();
      }
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::ShareRequest>().soft_delete(sr.id, mutation_context());
        txn->commit();
      }
      // Default query (status = 'pending') must not return revoked record.
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        const auto results = txn->repo<core::ShareRequest>().query(Query<core::ShareRequest>{});
        EXPECT_TRUE(results.empty());
      }
      // include_tombstoned() shows the revoked record.
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        const auto results =
            txn->repo<core::ShareRequest>().query(Query<core::ShareRequest>{}.include_tombstoned());
        ASSERT_EQ(results.size(), 1u);
        EXPECT_EQ(results.front().id, sr.id);
        EXPECT_EQ(results.front().status, core::ShareRequestStatus::Revoked);
        EXPECT_TRUE(results.front().decided_at.has_value());
      }
    }

    TEST_F(SqliteShareRequestRepositoryTest, QueryBySourceLabId) {
      const auto sr1 = make_share_request(1, lab1_, lab2_, user1_);
      const auto sr2 = make_share_request(2, lab2_, lab1_, user2_);
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::ShareRequest>().insert(sr1, mutation_context());
        txn->repo<core::ShareRequest>().insert(sr2, mutation_context());
        txn->commit();
      }
      auto txn = backend().begin(IsolationLevel::Serializable);
      const auto results = txn->repo<core::ShareRequest>().query(Query<core::ShareRequest>::where(
          field<core::ShareRequest, std::string>(core::ShareRequest::Field::SourceLabId) ==
          lab1_.to_string()));
      ASSERT_EQ(results.size(), 1u);
      EXPECT_EQ(results.front().id, sr1.id);
    }

    TEST_F(SqliteShareRequestRepositoryTest, QueryByTargetLabId) {
      const auto sr1 = make_share_request(1, lab1_, lab2_, user1_);
      const auto sr2 = make_share_request(2, lab2_, lab1_, user2_);
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::ShareRequest>().insert(sr1, mutation_context());
        txn->repo<core::ShareRequest>().insert(sr2, mutation_context());
        txn->commit();
      }
      auto txn = backend().begin(IsolationLevel::Serializable);
      const auto results = txn->repo<core::ShareRequest>().query(Query<core::ShareRequest>::where(
          field<core::ShareRequest, std::string>(core::ShareRequest::Field::TargetLabId) ==
          lab1_.to_string()));
      ASSERT_EQ(results.size(), 1u);
      EXPECT_EQ(results.front().id, sr2.id);
    }

    TEST_F(SqliteShareRequestRepositoryTest, ShareRequestUpdatePersistsChanges) {
      const auto sr = make_share_request(1, lab1_, lab2_, user1_);
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::ShareRequest>().insert(sr, mutation_context());
        txn->commit();
      }
      auto updated = sr;
      updated.scope_json = R"({"project_ids":["abc","def"]})";
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::ShareRequest>().update(updated, mutation_context());
        txn->commit();
      }
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        const auto found = txn->repo<core::ShareRequest>().find_by_id(sr.id);
        txn->commit();
        ASSERT_TRUE(found.has_value());
        EXPECT_EQ(found->scope_json, R"({"project_ids":["abc","def"]})");
      }
    }

    TEST_F(SqliteShareRequestRepositoryTest, ShareRequestRejectsEmptyScopeJson) {
      auto sr = make_share_request(1, lab1_, lab2_, user1_);
      sr.scope_json = "";
      auto txn = backend().begin(IsolationLevel::Serializable);
      EXPECT_THROW(txn->repo<core::ShareRequest>().insert(sr, mutation_context()),
                   ConstraintViolation);
    }

    TEST_F(SqliteShareRequestRepositoryTest, ShareRequestFindByNonexistentIdReturnsEmpty) {
      const auto fake_id = id_from_low<core::ShareRequestId>(99999);
      auto txn = backend().begin(IsolationLevel::Serializable);
      const auto found = txn->repo<core::ShareRequest>().find_by_id(fake_id);
      EXPECT_FALSE(found.has_value());
    }

    // ---- ShareRequestApproval tests ----

    TEST_F(SqliteShareRequestRepositoryTest, ApprovalInsertAndFindByCompositeKey) {
      const auto sr = make_share_request(1, lab1_, lab2_, user1_);
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::ShareRequest>().insert(sr, mutation_context());
        txn->commit();
      }
      const auto approval = make_approval(sr.id, core::ShareApprovalRole::SourceAdmin, user1_);
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::ShareRequestApproval>().insert(approval, mutation_context());
        txn->commit();
      }
      auto txn = backend().begin(IsolationLevel::Serializable);
      const auto found = txn->repo<core::ShareRequestApproval>().find_by_id(approval.id());
      ASSERT_TRUE(found.has_value());
      EXPECT_EQ(found->share_request_id, sr.id);
      EXPECT_EQ(found->approver_role, core::ShareApprovalRole::SourceAdmin);
      EXPECT_EQ(found->approver_user_id, user1_);
    }

    TEST_F(SqliteShareRequestRepositoryTest, DuplicateApprovalRoleThrowsUniqueViolation) {
      const auto sr = make_share_request(1, lab1_, lab2_, user1_);
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::ShareRequest>().insert(sr, mutation_context());
        txn->commit();
      }
      const auto approval = make_approval(sr.id, core::ShareApprovalRole::TargetAdmin, user2_);
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::ShareRequestApproval>().insert(approval, mutation_context());
        txn->commit();
      }
      auto txn = backend().begin(IsolationLevel::Serializable);
      EXPECT_THROW(txn->repo<core::ShareRequestApproval>().insert(approval, mutation_context()),
                   UniqueViolation);
    }

    TEST_F(SqliteShareRequestRepositoryTest, ApprovalUpdateThrowsUnsupportedOperation) {
      const auto sr = make_share_request(1, lab1_, lab2_, user1_);
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::ShareRequest>().insert(sr, mutation_context());
        txn->commit();
      }
      const auto approval = make_approval(sr.id, core::ShareApprovalRole::SystemAdmin, user1_);
      auto txn = backend().begin(IsolationLevel::Serializable);
      EXPECT_THROW(txn->repo<core::ShareRequestApproval>().update(approval, mutation_context()),
                   UnsupportedOperation);
    }

    TEST_F(SqliteShareRequestRepositoryTest, ApprovalSoftDeleteThrowsUnsupportedOperation) {
      auto txn = backend().begin(IsolationLevel::Serializable);
      const core::ShareRequestApprovalId dummy{id_from_low<core::ShareRequestId>(1),
                                               core::ShareApprovalRole::SourceAdmin};
      EXPECT_THROW(txn->repo<core::ShareRequestApproval>().soft_delete(dummy, mutation_context()),
                   UnsupportedOperation);
    }

    TEST_F(SqliteShareRequestRepositoryTest, QueryApprovalsByShareRequestId) {
      const auto sr = make_share_request(1, lab1_, lab2_, user1_);
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::ShareRequest>().insert(sr, mutation_context());
        txn->commit();
      }
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::ShareRequestApproval>().insert(
            make_approval(sr.id, core::ShareApprovalRole::SourceAdmin, user1_), mutation_context());
        txn->repo<core::ShareRequestApproval>().insert(
            make_approval(sr.id, core::ShareApprovalRole::TargetAdmin, user2_), mutation_context());
        txn->commit();
      }
      auto txn = backend().begin(IsolationLevel::Serializable);
      const auto results =
          txn->repo<core::ShareRequestApproval>().query(Query<core::ShareRequestApproval>::where(
              field<core::ShareRequestApproval, std::string>(
                  core::ShareRequestApproval::Field::ShareRequestId) == sr.id.to_string()));
      EXPECT_EQ(results.size(), 2u);
    }

    TEST_F(SqliteShareRequestRepositoryTest,
           ApprovalForNonExistentShareRequestThrowsConstraintViolation) {
      const auto fake_sr_id = id_from_low<core::ShareRequestId>(9999);
      const auto approval = make_approval(fake_sr_id, core::ShareApprovalRole::SourceAdmin, user1_);
      auto txn = backend().begin(IsolationLevel::Serializable);
      EXPECT_THROW(txn->repo<core::ShareRequestApproval>().insert(approval, mutation_context()),
                   ConstraintViolation);
    }

    TEST_F(SqliteShareRequestRepositoryTest, AllThreeApprovalRolesCanBeInserted) {
      const auto sr = make_share_request(1, lab1_, lab2_, user1_);
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::ShareRequest>().insert(sr, mutation_context());
        txn->commit();
      }
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::ShareRequestApproval>().insert(
            make_approval(sr.id, core::ShareApprovalRole::SourceAdmin, user1_), mutation_context());
        txn->repo<core::ShareRequestApproval>().insert(
            make_approval(sr.id, core::ShareApprovalRole::TargetAdmin, user2_), mutation_context());
        txn->repo<core::ShareRequestApproval>().insert(
            make_approval(sr.id, core::ShareApprovalRole::SystemAdmin, user1_), mutation_context());
        txn->commit();
      }
      auto txn = backend().begin(IsolationLevel::Serializable);
      const auto results =
          txn->repo<core::ShareRequestApproval>().query(Query<core::ShareRequestApproval>::where(
              field<core::ShareRequestApproval, std::string>(
                  core::ShareRequestApproval::Field::ShareRequestId) == sr.id.to_string()));
      EXPECT_EQ(results.size(), 3u);
    }

  } // namespace
} // namespace fmgr::storage
