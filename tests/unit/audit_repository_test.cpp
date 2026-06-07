// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/audit_event.h"
#include "core/identity.h"
#include "storage/AuditTraits.h"
#include "storage/IdentityTraits.h"
#include "storage/postgres/AuditRepositories.h"
#include "storage/postgres/IdentityRepositories.h"
#include "storage/sqlite/AuditRepositories.h"
#include "storage/sqlite/IdentityRepositories.h"

#include "repo_backend_harness.h"
#include "test_helpers.h"
#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>

namespace fmgr::storage {
  namespace {
    using namespace fmgr::test;

    [[nodiscard]] MutationContext mutation_context() {
      return MutationContext{
          .actor_user_id = id_from_low<core::UserId>(900),
          .actor_session_id = "audit-session",
          .request_id = "audit-request",
          .reason = "audit repository test",
          .lab_id = id_from_low<core::LabId>(1).to_string(),
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

    class AuditRepositoryTest : public ::testing::TestWithParam<BackendKind> {
    protected:
      void SetUp() override {
        if (GetParam() == BackendKind::Postgres && !postgres_test_url().has_value()) {
          GTEST_SKIP() << "FMGR_TEST_POSTGRES_URL not set; skipping Postgres repository tests";
        }
        harness_ = std::make_unique<RepoBackendHarness>(
            GetParam(),
            [](SqliteBackend& backend) {
              register_identity_repositories(backend);
              register_audit_repositories(backend);
            },
            [](PostgresBackend& backend) {
              register_identity_repositories(backend);
              register_audit_repositories(backend);
            });
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

    TEST_P(AuditRepositoryTest, CommitAppendsQueryableAuditRow) {
      const auto lab = make_lab(1);
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Lab>().insert(lab, mutation_context());
        txn->commit();
      }
      EXPECT_EQ(audit_event_count(), 1U);

      auto txn = backend().begin(IsolationLevel::Serializable);
      const auto events = txn->repo<core::AuditEvent>().query(Query<core::AuditEvent>::all());
      txn->commit();
      ASSERT_EQ(events.size(), 1U);
      EXPECT_EQ(events.front().entity_kind, "lab");
      EXPECT_EQ(events.front().entity_id, lab.id.to_string());
      EXPECT_FALSE(events.front().this_hash.empty());
    }

    TEST_P(AuditRepositoryTest, QueryFiltersByEntityId) {
      const auto lab_a = make_lab(1);
      const auto lab_b = make_lab(2);
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Lab>().insert(lab_a, mutation_context());
        txn->commit();
      }
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Lab>().insert(lab_b, mutation_context());
        txn->commit();
      }

      auto txn = backend().begin(IsolationLevel::Serializable);
      const auto events = txn->repo<core::AuditEvent>().query(Query<core::AuditEvent>::where(
          field<core::AuditEvent, std::string>(core::AuditEvent::Field::EntityId) ==
          lab_b.id.to_string()));
      txn->commit();
      ASSERT_EQ(events.size(), 1U);
      EXPECT_EQ(events.front().entity_id, lab_b.id.to_string());
    }

    TEST_P(AuditRepositoryTest, AppendOnlyMutationsThrowUnsupportedOperation) {
      auto txn = backend().begin(IsolationLevel::Serializable);
      auto& repo = txn->repo<core::AuditEvent>();
      const core::AuditEvent event{
          .id = id_from_low<core::AuditEventId>(7),
          .at = ts(1),
          .actor_user_id = id_from_low<core::UserId>(1),
          .actor_session_id = "s",
          .lab_id = std::nullopt,
          .action = "mutation",
          .entity_kind = "lab",
          .entity_id = std::nullopt,
          .before_json = "{}",
          .after_json = "{}",
          .request_id = "r",
          .prev_hash = std::string(64, '0'),
          .this_hash = std::string(64, '1'),
      };
      EXPECT_THROW(repo.insert(event, mutation_context()), UnsupportedOperation);
      EXPECT_THROW(repo.update(event, mutation_context()), UnsupportedOperation);
      EXPECT_THROW(repo.soft_delete(event.id, mutation_context()), UnsupportedOperation);
      txn->rollback();
    }

    INSTANTIATE_TEST_SUITE_P(Backends, AuditRepositoryTest,
                             ::testing::Values(BackendKind::Sqlite, BackendKind::Postgres),
                             [](const ::testing::TestParamInfo<BackendKind>& info) {
                               return backend_kind_name(info.param);
                             });

  } // namespace
} // namespace fmgr::storage
