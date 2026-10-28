// SPDX-License-Identifier: AGPL-3.0-or-later

#include "audit/AuditChainVerifier.h"
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

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

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

    // Corrupt pre-hardening fixtures deliberately bypass the read-only audit
    // repository. Production code never uses these privileged fixture seams.
    void fixture_sql(ITransaction& txn, const std::string& sql) {
      if (auto* sqlite = dynamic_cast<SqliteTransaction*>(&txn)) {
        sqlite->add_commit_hook([sql](sqlite3* handle) {
          if (sqlite3_exec(handle, sql.c_str(), nullptr, nullptr, nullptr) != SQLITE_OK) {
            throw std::runtime_error("audit fixture SQL failed");
          }
        });
      } else {
        dynamic_cast<PostgresTransaction&>(txn).work().exec(sql);
      }
    }

    [[nodiscard]] core::AuditEvent fixture_event(std::uint64_t low_bits, std::string previous) {
      core::AuditEvent event{
          .id = id_from_low<core::AuditEventId>(low_bits),
          .at = ts(static_cast<std::int64_t>(low_bits)),
          .actor_user_id = id_from_low<core::UserId>(900),
          .actor_session_id = "fixture-session",
          .action = "fixture",
          .entity_kind = "lab",
          .entity_id = std::nullopt,
          .request_id = "fixture-request",
          .prev_hash = std::move(previous),
      };
      event.this_hash =
          audit::compute_audit_hash(event.prev_hash, audit::audit_event_content_json(event));
      return event;
    }

    void insert_fixture_event(ITransaction& txn, const core::AuditEvent& event) {
      const std::string sql =
          "INSERT INTO audit_events "
          "(id,at_micros,actor_user_id,actor_session_id,lab_id,action,entity_kind,entity_id,"
          "before_json,after_json,request_id,prev_hash,this_hash) VALUES ('" +
          event.id.to_string() + "'," + std::to_string(event.at.unix_micros()) + ",'" +
          event.actor_user_id.to_string() +
          "','fixture-session',NULL,'fixture','lab',NULL,"
          "'{}','{}','fixture-request','" +
          event.prev_hash + "','" + event.this_hash + "')";
      fixture_sql(txn, sql);
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

      [[nodiscard]] std::vector<core::AuditEvent> events() {
        auto txn = backend().begin(IsolationLevel::ReadCommitted);
        auto rows = txn->repo<core::AuditEvent>().query(Query<core::AuditEvent>::all());
        txn->commit();
        std::ranges::sort(rows, {}, &core::AuditEvent::id);
        return rows;
      }

      void emulate_legacy_schema() {
        auto txn = backend().begin(IsolationLevel::Serializable);
        fixture_sql(*txn, GetParam() == BackendKind::Sqlite
                              ? "DROP INDEX IF EXISTS audit_events_prev_hash_unique"
                              : "ALTER TABLE audit_events DROP CONSTRAINT IF EXISTS "
                                "audit_events_prev_hash_unique");
        fixture_sql(*txn, "DELETE FROM schema_migrations WHERE version = 16");
        txn->commit();
        ASSERT_EQ(backend().current_version(), SchemaVersion{15});
      }

      void require_migration_refuses(const std::vector<core::AuditEvent>& bad_chain) {
        emulate_legacy_schema();
        auto txn = backend().begin(IsolationLevel::Serializable);
        for (const auto& event : bad_chain) {
          insert_fixture_event(*txn, event);
        }
        txn->commit();
        const auto before = events();
        EXPECT_THROW(backend().migrate_to_latest(), MigrationFailure);
        EXPECT_EQ(backend().current_version(), SchemaVersion{15});
        EXPECT_EQ(events(), before);
        // Failed preflight releases all locks, and the next startup still
        // refuses the unchanged invalid chain rather than picking a branch.
        EXPECT_THROW(backend().migrate_to_latest(), MigrationFailure);
        EXPECT_EQ(events(), before);
        EXPECT_EQ(backend().current_version(), SchemaVersion{15});
      }

      void install_append_trigger(const std::string& body) {
        auto txn = backend().begin(IsolationLevel::ReadCommitted);
        fixture_sql(*txn, "CREATE FUNCTION force_audit_outcome() RETURNS trigger LANGUAGE plpgsql "
                          "AS $$ BEGIN " +
                              body +
                              " RETURN NEW; END; $$; "
                              "CREATE TRIGGER force_audit_outcome BEFORE INSERT ON audit_events "
                              "FOR EACH ROW EXECUTE FUNCTION force_audit_outcome()");
        txn->commit();
      }

      template <typename Error> void require_failed_append_rolls_back() {
        const auto before = events();
        auto txn = backend().begin(IsolationLevel::ReadCommitted);
        txn->repo<core::Lab>().insert(make_lab(10), mutation_context());
        txn->note_mutation("force_failure", "test", mutation_context());
        EXPECT_THROW(txn->commit(), Error);
        auto read = backend().begin(IsolationLevel::ReadCommitted);
        EXPECT_FALSE(read->repo<core::Lab>().find_by_id(make_lab(10).id).has_value());
        read->commit();
        EXPECT_EQ(events(), before);
        EXPECT_TRUE(audit::verify_audit_chain(events()).ok);
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

    TEST_P(AuditRepositoryTest, AuditIntegrityMigrationAcceptsEmptyChainWithoutBackfill) {
      emulate_legacy_schema();
      ASSERT_NO_THROW(backend().migrate_to_latest());
      EXPECT_EQ(backend().current_version(), SchemaVersion{16});
      EXPECT_TRUE(events().empty());
    }

    TEST_P(AuditRepositoryTest, AuditIntegrityMigrationPreservesValidExistingChain) {
      emulate_legacy_schema();
      auto first = fixture_event(1, std::string(audit::zero_hash()));
      const auto second = fixture_event(2, first.this_hash);
      auto txn = backend().begin(IsolationLevel::ReadCommitted);
      insert_fixture_event(*txn, first);
      insert_fixture_event(*txn, second);
      txn->commit();
      const auto before = events();
      ASSERT_NO_THROW(backend().migrate_to_latest());
      EXPECT_EQ(backend().current_version(), SchemaVersion{16});
      EXPECT_EQ(events(), before);
      EXPECT_TRUE(audit::verify_audit_chain(events()).ok);
    }

    TEST_P(AuditRepositoryTest, AuditIntegrityMigrationRejectsForkedGenesisAtomically) {
      require_migration_refuses({fixture_event(1, std::string(audit::zero_hash())),
                                 fixture_event(2, std::string(audit::zero_hash()))});
    }

    TEST_P(AuditRepositoryTest, AuditIntegrityMigrationRejectsNonGenesisForkAtomically) {
      const auto head = fixture_event(1, std::string(audit::zero_hash()));
      require_migration_refuses(
          {head, fixture_event(2, head.this_hash), fixture_event(3, head.this_hash)});
    }

    TEST_P(AuditRepositoryTest, AuditIntegrityMigrationRejectsOrphanAtomically) {
      require_migration_refuses({fixture_event(1, std::string(audit::zero_hash())),
                                 fixture_event(2, std::string(64, 'f'))});
    }

    TEST_P(AuditRepositoryTest, AuditIntegrityMigrationRejectsHashMismatchAtomically) {
      auto tampered = fixture_event(1, std::string(audit::zero_hash()));
      tampered.this_hash = std::string(64, 'a');
      require_migration_refuses({tampered});
    }

    TEST_P(AuditRepositoryTest, VerificationRequirementIsBoundToAppliedMigrationChecksum) {
      std::string connection;
      {
        auto txn = backend().begin(IsolationLevel::ReadCommitted);
        if (GetParam() == BackendKind::Sqlite) {
          connection = sqlite3_db_filename(dynamic_cast<SqliteTransaction&>(*txn).handle(), "main");
        } else {
          connection = dynamic_cast<PostgresTransaction&>(*txn).work().conn().connection_string();
        }
        txn->commit();
      }
      const auto with_requirement =
          [&](bool require_verification) -> std::unique_ptr<IStorageBackend> {
        if (GetParam() == BackendKind::Sqlite) {
          return std::make_unique<SqliteBackend>(SqliteBackendOptions{
              .database_path = connection,
              .migrations = {{.version = 17,
                              .name = "verified_test",
                              .up_sql = "SELECT 1;",
                              .requires_verified_audit_chain = require_verification}},
          });
        }
        return std::make_unique<PostgresBackend>(PostgresBackendOptions{
            .connection_string = connection,
            .pool_size = 1,
            .migrations = {{.version = 17,
                            .name = "verified_test",
                            .up_sql = "SELECT 1;",
                            .requires_verified_audit_chain = require_verification}},
        });
      };
      auto verified = with_requirement(true);
      ASSERT_NO_THROW(verified->migrate_to_latest());
      auto removed_requirement = with_requirement(false);
      EXPECT_THROW(removed_requirement->migrate_to_latest(), MigrationFailure);
      EXPECT_EQ(removed_requirement->current_version(), SchemaVersion{17});
    }

    TEST_P(AuditRepositoryTest, PredecessorUniquenessIncludesGenesisAndLaterRows) {
      for (const bool genesis : {true, false}) {
        SCOPED_TRACE(genesis);
        const auto first = fixture_event(genesis ? 1 : 3, genesis ? std::string(audit::zero_hash())
                                                                  : events().front().this_hash);
        {
          auto txn = backend().begin(IsolationLevel::ReadCommitted);
          insert_fixture_event(*txn, first);
          txn->commit();
        }
        const auto before = events();
        const auto conflicting = fixture_event(genesis ? 2 : 4, first.prev_hash);
        EXPECT_THROW(
            {
              auto txn = backend().begin(IsolationLevel::ReadCommitted);
              insert_fixture_event(*txn, conflicting);
              txn->commit();
            },
            std::exception);
        EXPECT_EQ(events(), before);
      }
    }

    TEST_P(AuditRepositoryTest, NamedPredecessorNoInsertIsSerializationFailureAndRollsBackBatch) {
      if (GetParam() != BackendKind::Postgres) {
        GTEST_SKIP() << "PostgreSQL named conflict result-count contract";
      }
      install_append_trigger(
          "IF pg_trigger_depth() = 1 AND NEW.entity_kind = 'force_failure' THEN "
          "INSERT INTO audit_events "
          "(id,at_micros,actor_user_id,actor_session_id,lab_id,action,entity_kind,entity_id,"
          "before_json,after_json,request_id,prev_hash,this_hash) VALUES "
          "('00000000-0000-0000-0000-000000000042',NEW.at_micros,NEW.actor_user_id,"
          "NEW.actor_session_id,NEW.lab_id,NEW.action,'fixture',NEW.entity_id,NEW.before_json,"
          "NEW.after_json,NEW.request_id,NEW.prev_hash,repeat('b',64)); END IF;");
      require_failed_append_rolls_back<SerializationFailure>();
    }

    TEST_P(AuditRepositoryTest, PredecessorConstraintFailureRollsBackEarlierDomainWrites) {
      const auto first = fixture_event(1, std::string(audit::zero_hash()));
      const auto fork = fixture_event(2, first.prev_hash);
      EXPECT_THROW(
          {
            auto txn = backend().begin(IsolationLevel::ReadCommitted);
            txn->repo<core::Lab>().insert(make_lab(10), mutation_context());
            insert_fixture_event(*txn, first);
            insert_fixture_event(*txn, fork);
            txn->commit();
          },
          std::exception);
      auto read = backend().begin(IsolationLevel::ReadCommitted);
      EXPECT_FALSE(read->repo<core::Lab>().find_by_id(make_lab(10).id).has_value());
      read->commit();
      EXPECT_TRUE(events().empty());
    }

    TEST_P(AuditRepositoryTest, NativeSerializationFailureRollsBackEarlierAuditAndDomainRows) {
      if (GetParam() != BackendKind::Postgres) {
        GTEST_SKIP() << "PostgreSQL SQLSTATE contract";
      }
      install_append_trigger(
          "IF NEW.entity_kind = 'force_failure' THEN RAISE EXCEPTION "
          "USING ERRCODE = '40001', MESSAGE = 'injected serialization failure'; END IF;");
      require_failed_append_rolls_back<SerializationFailure>();
    }

    TEST_P(AuditRepositoryTest, NativeDeadlockFailureRollsBackEarlierAuditAndDomainRows) {
      if (GetParam() != BackendKind::Postgres) {
        GTEST_SKIP() << "PostgreSQL SQLSTATE contract";
      }
      install_append_trigger("IF NEW.entity_kind = 'force_failure' THEN RAISE EXCEPTION "
                             "USING ERRCODE = '40P01', MESSAGE = 'injected deadlock'; END IF;");
      require_failed_append_rolls_back<SerializationFailure>();
    }

    TEST_P(AuditRepositoryTest, UnrelatedAuditHashUniquenessRemainsUniqueViolation) {
      if (GetParam() != BackendKind::Postgres) {
        GTEST_SKIP() << "PostgreSQL named conflict target excludes this_hash uniqueness";
      }
      install_append_trigger("IF NEW.entity_kind = 'force_failure' THEN NEW.this_hash := "
                             "(SELECT this_hash FROM audit_events LIMIT 1); END IF;");
      require_failed_append_rolls_back<UniqueViolation>();
    }

    // The repository (not the caller) derives before/after audit snapshots from
    // authoritative entity state — MutationContext no longer carries them.
    TEST_P(AuditRepositoryTest, RepositoryDerivesInsertAndUpdateSnapshots) {
      auto lab = make_lab(1);
      lab.name = "Original Lab";
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Lab>().insert(lab, mutation_context());
        txn->commit();
      }
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        lab.name = "Renamed Lab";
        txn->repo<core::Lab>().update(lab, mutation_context());
        txn->commit();
      }

      auto txn = backend().begin(IsolationLevel::Serializable);
      const auto events = txn->repo<core::AuditEvent>().query(Query<core::AuditEvent>::where(
          field<core::AuditEvent, std::string>(core::AuditEvent::Field::EntityId) ==
          lab.id.to_string()));
      txn->commit();
      ASSERT_EQ(events.size(), 2U);

      const auto find_action = [&events](std::string_view action) {
        const auto iter =
            std::find_if(events.begin(), events.end(),
                         [&](const core::AuditEvent& event) { return event.action == action; });
        return iter;
      };
      const auto insert_event = find_action("insert");
      const auto update_event = find_action("update");
      ASSERT_NE(insert_event, events.end());
      ASSERT_NE(update_event, events.end());

      EXPECT_EQ(insert_event->before_json, "{}"); // inserts have no before image
      EXPECT_NE(insert_event->after_json.find("Original Lab"), std::string::npos);

      // The before image is the authoritative pre-update row, not caller-supplied.
      EXPECT_NE(update_event->before_json.find("Original Lab"), std::string::npos);
      EXPECT_NE(update_event->after_json.find("Renamed Lab"), std::string::npos);
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

    using SnapshotCase = std::tuple<IsolationLevel, bool, bool, bool>;

    class AuditSnapshotTest : public ::testing::TestWithParam<SnapshotCase> {
    protected:
      void SetUp() override {
        if (!postgres_test_url().has_value()) {
          GTEST_SKIP() << "FMGR_TEST_POSTGRES_URL not set; live PostgreSQL test not run";
        }
        harness_ = std::make_unique<RepoBackendHarness>(
            BackendKind::Postgres, [](SqliteBackend&) {},
            [](PostgresBackend& backend) {
              register_identity_repositories(backend);
              register_audit_repositories(backend);
            });
      }

      [[nodiscard]] IStorageBackend& backend() {
        return harness_->backend();
      }

      [[nodiscard]] std::vector<core::AuditEvent> events() {
        auto txn = backend().begin(IsolationLevel::ReadCommitted);
        auto rows = txn->repo<core::AuditEvent>().query(Query<core::AuditEvent>::all());
        txn->commit();
        return rows;
      }

      std::unique_ptr<RepoBackendHarness> harness_;
    };

    TEST_P(AuditSnapshotTest, MixedIsolationWritersKeepOneChainAndRollbackLosingDomainBatch) {
      const auto [isolation, nonempty, batch, reverse] = GetParam();
      if (nonempty) {
        auto seed = backend().begin(IsolationLevel::ReadCommitted);
        seed->repo<core::Lab>().insert(make_lab(1), mutation_context());
        seed->commit();
      }
      const auto seed_count = events().size();

      // These two live transactions are the barrier: A establishes its old
      // snapshot and stages a domain batch, then B fully commits before A is
      // allowed into the common audit append. There is no scheduling or sleep.
      auto old = backend().begin(isolation);
      (void)old->repo<core::AuditEvent>().query(Query<core::AuditEvent>::all());
      old->repo<core::Lab>().insert(make_lab(10), mutation_context());
      if (batch) {
        old->repo<core::Lab>().insert(make_lab(11), mutation_context());
      }
      auto fresh = backend().begin(IsolationLevel::ReadCommitted);
      fresh->repo<core::Lab>().insert(make_lab(20), mutation_context());
      if (reverse) {
        old->commit();
        fresh->commit();
      } else {
        fresh->commit();
        EXPECT_THROW(old->commit(), SerializationFailure);
        auto read = backend().begin(IsolationLevel::ReadCommitted);
        EXPECT_FALSE(read->repo<core::Lab>().find_by_id(make_lab(10).id).has_value());
        EXPECT_FALSE(read->repo<core::Lab>().find_by_id(make_lab(11).id).has_value());
        read->commit();
        const auto after_failure = events();
        EXPECT_EQ(after_failure.size(), seed_count + 1);
        EXPECT_TRUE(audit::verify_audit_chain(after_failure).ok);

        // Retrying the entire operation in a fresh snapshot succeeds.
        auto retry = backend().begin(isolation);
        retry->repo<core::Lab>().insert(make_lab(10), mutation_context());
        if (batch) {
          retry->repo<core::Lab>().insert(make_lab(11), mutation_context());
        }
        retry->commit();
      }
      const auto final = events();
      EXPECT_EQ(final.size(), seed_count + (batch ? 3 : 2));
      EXPECT_TRUE(audit::verify_audit_chain(final).ok);
    }

    [[nodiscard]] std::string
    snapshot_case_name(const ::testing::TestParamInfo<SnapshotCase>& info) {
      const auto [isolation, nonempty, batch, reverse] = info.param;
      return std::string(isolation == IsolationLevel::Serializable ? "Serializable"
                                                                   : "RepeatableRead") +
             (nonempty ? "ExistingChain" : "Genesis") + (batch ? "Batch" : "Single") +
             (reverse ? "OldFirst" : "NewFirst");
    }

    INSTANTIATE_TEST_SUITE_P(PostgreSql, AuditSnapshotTest,
                             ::testing::Combine(::testing::Values(IsolationLevel::Serializable,
                                                                  IsolationLevel::RepeatableRead),
                                                ::testing::Bool(), ::testing::Bool(),
                                                ::testing::Bool()),
                             snapshot_case_name);

  } // namespace
} // namespace fmgr::storage
