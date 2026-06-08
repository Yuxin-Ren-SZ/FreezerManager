// SPDX-License-Identifier: AGPL-3.0-or-later

#include "audit/CanonicalJson.h"
#include "core/audit_event.h"
#include "core/ids.h"
#include "core/timestamp.h"
#include "storage/AuditTraits.h"
#include "storage/IStorageBackend.h"
#include "storage/sqlite/AuditRepositories.h"
#include "storage/sqlite/SqliteBackend.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <optional>
#include <string>
#include <vector>

namespace fmgr {
  namespace {

    // ---- CanonicalJson tests ----

    TEST(CanonicalJson, EmptyObjectProducesEmptyBraces) {
      EXPECT_EQ(audit::canonical_json(nlohmann::json::object()), "{}");
    }

    TEST(CanonicalJson, ArrayPreservesOrder) {
      EXPECT_EQ(audit::canonical_json(nlohmann::json::array({3, 1, 2})), "[3,1,2]");
    }

    TEST(CanonicalJson, ObjectKeysAreSortedAlphabetically) {
      const auto value = nlohmann::json::parse(R"({"zebra":1,"apple":2,"mango":3})");
      EXPECT_EQ(audit::canonical_json(value), R"({"apple":2,"mango":3,"zebra":1})");
    }

    TEST(CanonicalJson, NestedObjectKeysAreSorted) {
      const auto value = nlohmann::json::parse(R"({"b":{"z":1,"a":2},"a":{"y":3,"x":4}})");
      EXPECT_EQ(audit::canonical_json(value), R"({"a":{"x":4,"y":3},"b":{"a":2,"z":1}})");
    }

    TEST(CanonicalJson, SameDataFromDifferentInsertionOrders) {
      const auto a = nlohmann::json::parse(R"({"id":"abc","at":123})");
      const auto b = nlohmann::json::parse(R"({"at":123,"id":"abc"})");
      EXPECT_EQ(audit::canonical_json(a), audit::canonical_json(b));
    }

    TEST(CanonicalJson, NullBoolIntStringRoundTrip) {
      const nlohmann::json value = {{"a", nullptr}, {"b", true}, {"c", 42}, {"d", "hello"}};
      EXPECT_EQ(audit::canonical_json(value), R"({"a":null,"b":true,"c":42,"d":"hello"})");
    }

    TEST(CanonicalJson, CompactOutputHasNoExtraWhitespace) {
      const nlohmann::json value = {{"key", "value"}};
      const auto result = audit::canonical_json(value);
      // No space after : or ,
      EXPECT_EQ(result.find(' '), std::string::npos);
    }

    // ---- compute_audit_hash tests ----

    TEST(AuditHash, ZeroHashConstantIs64HexZeros) {
      ASSERT_EQ(audit::zero_hash().size(), 64U);
      for (const char ch : audit::zero_hash()) {
        EXPECT_EQ(ch, '0');
      }
    }

    TEST(AuditHash, ComputeHashIsDeterministic) {
      const auto h1 = audit::compute_audit_hash(audit::zero_hash(), R"({"x":1})");
      const auto h2 = audit::compute_audit_hash(audit::zero_hash(), R"({"x":1})");
      EXPECT_EQ(h1, h2);
    }

    TEST(AuditHash, DifferentPrevHashProducesDifferentResult) {
      const auto h1 = audit::compute_audit_hash(audit::zero_hash(), R"({"x":1})");
      const std::string different(64, '1');
      const auto h2 = audit::compute_audit_hash(different, R"({"x":1})");
      EXPECT_NE(h1, h2);
    }

    TEST(AuditHash, DifferentContentProducesDifferentResult) {
      const auto h1 = audit::compute_audit_hash(audit::zero_hash(), R"({"x":1})");
      const auto h2 = audit::compute_audit_hash(audit::zero_hash(), R"({"x":2})");
      EXPECT_NE(h1, h2);
    }

    TEST(AuditHash, OutputIs64LowercaseHexChars) {
      const auto hash = audit::compute_audit_hash(audit::zero_hash(), "{}");
      ASSERT_EQ(hash.size(), 64U);
      for (const char ch : hash) {
        const bool valid = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
        EXPECT_TRUE(valid) << "non-lowercase-hex char: " << ch;
      }
    }

    // ---- AuditEvent domain type tests ----

    TEST(AuditEvent, JsonRoundTripWithAllFields) {
      const core::AuditEvent event{
          .id = core::AuditEventId::parse("11111111-1111-1111-1111-111111111111"),
          .at = core::Timestamp::from_unix_micros(1000000),
          .actor_user_id = core::UserId::parse("22222222-2222-2222-2222-222222222222"),
          .actor_session_id = "sess-abc",
          .lab_id = core::LabId::parse("33333333-3333-3333-3333-333333333333"),
          .action = "insert",
          .entity_kind = "lab",
          .entity_id = std::optional<std::string>{"33333333-3333-3333-3333-333333333333"},
          .before_json = "{}",
          .after_json = R"({"name":"BioLab"})",
          .request_id = "req-001",
          .prev_hash = std::string(audit::zero_hash()),
          .this_hash = audit::compute_audit_hash(audit::zero_hash(), "{}"),
      };
      const nlohmann::json json = event;
      const auto roundtripped = json.get<core::AuditEvent>();
      EXPECT_EQ(event, roundtripped);
    }

    TEST(AuditEvent, NullOptionalFieldsRoundTrip) {
      core::AuditEvent event{};
      event.id = core::AuditEventId::parse("44444444-4444-4444-4444-444444444444");
      event.at = core::Timestamp::from_unix_micros(0);
      event.actor_user_id = core::UserId::parse("55555555-5555-5555-5555-555555555555");
      event.actor_session_id = "s";
      event.lab_id = std::nullopt;
      event.action = "mutation";
      event.entity_kind = "user";
      event.entity_id = std::nullopt;
      event.request_id = "r";
      event.prev_hash = std::string(audit::zero_hash());
      event.this_hash = std::string(64, '1');

      const nlohmann::json json = event;
      EXPECT_TRUE(json.at("lab_id").is_null());
      EXPECT_TRUE(json.at("entity_id").is_null());
      const auto roundtripped = json.get<core::AuditEvent>();
      EXPECT_EQ(event, roundtripped);
    }

    // ---- SQLite audit integration ----

    [[nodiscard]] storage::MutationContext test_ctx() {
      return storage::MutationContext{
          .actor_user_id = core::UserId::parse("aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa"),
          .actor_session_id = "test-session",
          .request_id = "req-test",
          .reason = "unit test",
      };
    }

    // Build a backend using the real default migrations (includes 0011_audit_events)
    // and registers the AuditEventRepository.
    [[nodiscard]] storage::SqliteBackend make_audit_backend() {
      storage::SqliteBackend backend(storage::SqliteBackendOptions{});
      storage::register_audit_repositories(backend);
      backend.migrate_to_latest();
      return backend;
    }

    TEST(SqliteAuditEvent, MigrationCreatesTableWithZeroRows) {
      const auto backend = make_audit_backend();
      EXPECT_EQ(backend.audit_event_count_for_tests(), 0U);
    }

    TEST(SqliteAuditEvent, AuditRowWrittenOnCommit) {
      auto backend = make_audit_backend();
      const auto ctx = test_ctx();

      {
        auto txn = backend.begin(storage::IsolationLevel::Serializable);
        auto& sqlite_txn = dynamic_cast<storage::SqliteTransaction&>(*txn);
        sqlite_txn.note_mutation("lab", "lab-abc", ctx);
        txn->commit();
      }

      EXPECT_EQ(backend.audit_event_count_for_tests(), 1U);

      auto read_txn = backend.begin(storage::IsolationLevel::ReadCommitted);
      const auto events =
          read_txn->repo<core::AuditEvent>().query(storage::Query<core::AuditEvent>::all());
      ASSERT_EQ(events.size(), 1U);
      const auto& ev = events.front();
      EXPECT_EQ(ev.actor_user_id, ctx.actor_user_id);
      EXPECT_EQ(ev.actor_session_id, ctx.actor_session_id);
      EXPECT_EQ(ev.request_id, ctx.request_id);
      EXPECT_EQ(ev.entity_kind, "lab");
      EXPECT_EQ(ev.entity_id, std::optional<std::string>{"lab-abc"});
      // First row chains from the zero hash.
      EXPECT_EQ(ev.prev_hash, std::string(audit::zero_hash()));
      // this_hash must be non-zero (it was computed from real content).
      EXPECT_NE(ev.this_hash, std::string(audit::zero_hash()));
    }

    TEST(SqliteAuditEvent, RollbackProducesNoAuditRow) {
      auto backend = make_audit_backend();
      {
        auto txn = backend.begin(storage::IsolationLevel::Serializable);
        auto& sqlite_txn = dynamic_cast<storage::SqliteTransaction&>(*txn);
        sqlite_txn.note_mutation("lab", "lab-rollback", test_ctx());
        txn->rollback();
      }
      EXPECT_EQ(backend.audit_event_count_for_tests(), 0U);
    }

    TEST(SqliteAuditEvent, FailNextAuditAppendRollsBackEverything) {
      auto backend = make_audit_backend();
      backend.fail_next_audit_append_for_tests();
      {
        auto txn = backend.begin(storage::IsolationLevel::Serializable);
        auto& sqlite_txn = dynamic_cast<storage::SqliteTransaction&>(*txn);
        sqlite_txn.note_mutation("lab", "lab-fail", test_ctx());
        EXPECT_THROW(txn->commit(), storage::ConstraintViolation);
      }
      EXPECT_EQ(backend.audit_event_count_for_tests(), 0U);
    }

    TEST(SqliteAuditEvent, ConsecutiveCommitsFormChain) {
      auto backend = make_audit_backend();
      {
        auto txn = backend.begin(storage::IsolationLevel::Serializable);
        dynamic_cast<storage::SqliteTransaction&>(*txn).note_mutation("lab", "l1", test_ctx());
        txn->commit();
      }
      {
        auto txn = backend.begin(storage::IsolationLevel::Serializable);
        dynamic_cast<storage::SqliteTransaction&>(*txn).note_mutation("user", "u1", test_ctx());
        txn->commit();
      }

      auto read_txn = backend.begin(storage::IsolationLevel::ReadCommitted);
      const auto events =
          read_txn->repo<core::AuditEvent>().query(storage::Query<core::AuditEvent>::all());
      ASSERT_EQ(events.size(), 2U);
      EXPECT_EQ(events[0].prev_hash, std::string(audit::zero_hash()));
      EXPECT_EQ(events[1].prev_hash, events[0].this_hash);
      EXPECT_NE(events[0].this_hash, events[1].this_hash);
    }

    TEST(SqliteAuditEvent, MultiMutationTransactionBuildsSubChain) {
      auto backend = make_audit_backend();
      {
        auto txn = backend.begin(storage::IsolationLevel::Serializable);
        auto& st = dynamic_cast<storage::SqliteTransaction&>(*txn);
        st.note_mutation("lab", "l-A", test_ctx());
        st.note_mutation("user", "u-A", test_ctx());
        txn->commit();
      }

      auto read_txn = backend.begin(storage::IsolationLevel::ReadCommitted);
      const auto events =
          read_txn->repo<core::AuditEvent>().query(storage::Query<core::AuditEvent>::all());
      ASSERT_EQ(events.size(), 2U);
      EXPECT_EQ(events[0].prev_hash, std::string(audit::zero_hash()));
      EXPECT_EQ(events[1].prev_hash, events[0].this_hash);
    }

    TEST(SqliteAuditEvent, UpdateTriggerRejectsModification) {
      auto backend = make_audit_backend();
      // First commit a row.
      {
        auto txn = backend.begin(storage::IsolationLevel::Serializable);
        dynamic_cast<storage::SqliteTransaction&>(*txn).note_mutation("lab", "l-trig", test_ctx());
        txn->commit();
      }
      ASSERT_EQ(backend.audit_event_count_for_tests(), 1U);

      // Attempt an UPDATE via a commit hook; verify the trigger rejects it.
      int update_result = SQLITE_OK;
      {
        auto txn = backend.begin(storage::IsolationLevel::Serializable);
        auto& st = dynamic_cast<storage::SqliteTransaction&>(*txn);
        st.add_commit_hook([&update_result](sqlite3* handle) {
          update_result = sqlite3_exec(handle, "UPDATE audit_events SET action = 'tampered'",
                                       nullptr, nullptr, nullptr);
        });
        txn->commit();
      }
      // SQLITE_CONSTRAINT (19) is the category; SQLITE_CONSTRAINT_TRIGGER (787) is the exact code.
      EXPECT_NE(update_result, SQLITE_OK);

      // Verify no row was actually changed.
      auto read_txn = backend.begin(storage::IsolationLevel::ReadCommitted);
      const auto events =
          read_txn->repo<core::AuditEvent>().query(storage::Query<core::AuditEvent>::all());
      ASSERT_EQ(events.size(), 1U);
      EXPECT_NE(events.front().action, "tampered");
    }

    TEST(SqliteAuditEvent, DeleteTriggerPreventsRowRemoval) {
      auto backend = make_audit_backend();
      {
        auto txn = backend.begin(storage::IsolationLevel::Serializable);
        dynamic_cast<storage::SqliteTransaction&>(*txn).note_mutation("lab", "l-del", test_ctx());
        txn->commit();
      }
      ASSERT_EQ(backend.audit_event_count_for_tests(), 1U);

      int delete_result = SQLITE_OK;
      {
        auto txn = backend.begin(storage::IsolationLevel::Serializable);
        auto& st = dynamic_cast<storage::SqliteTransaction&>(*txn);
        st.add_commit_hook([&delete_result](sqlite3* handle) {
          delete_result =
              sqlite3_exec(handle, "DELETE FROM audit_events", nullptr, nullptr, nullptr);
        });
        txn->commit();
      }
      EXPECT_NE(delete_result, SQLITE_OK);
      EXPECT_EQ(backend.audit_event_count_for_tests(), 1U);
    }

    TEST(SqliteAuditEvent, RepositoryInsertThrowsUnsupportedOperation) {
      auto backend = make_audit_backend();
      auto txn = backend.begin(storage::IsolationLevel::Serializable);
      core::AuditEvent dummy{};
      dummy.id = core::AuditEventId::parse("ffffffff-ffff-ffff-ffff-ffffffffffff");
      EXPECT_THROW(txn->repo<core::AuditEvent>().insert(dummy, test_ctx()),
                   storage::UnsupportedOperation);
    }

    TEST(SqliteAuditEvent, RepositoryUpdateThrowsUnsupportedOperation) {
      auto backend = make_audit_backend();
      auto txn = backend.begin(storage::IsolationLevel::Serializable);
      core::AuditEvent dummy{};
      dummy.id = core::AuditEventId::parse("ffffffff-ffff-ffff-ffff-ffffffffffff");
      EXPECT_THROW(txn->repo<core::AuditEvent>().update(dummy, test_ctx()),
                   storage::UnsupportedOperation);
    }

    TEST(SqliteAuditEvent, RepositorySoftDeleteThrowsUnsupportedOperation) {
      auto backend = make_audit_backend();
      auto txn = backend.begin(storage::IsolationLevel::Serializable);
      EXPECT_THROW(
          txn->repo<core::AuditEvent>().soft_delete(
              core::AuditEventId::parse("ffffffff-ffff-ffff-ffff-ffffffffffff"), test_ctx()),
          storage::UnsupportedOperation);
    }

    TEST(SqliteAuditEvent, FindByIdReturnsInsertedRow) {
      auto backend = make_audit_backend();
      {
        auto txn = backend.begin(storage::IsolationLevel::Serializable);
        dynamic_cast<storage::SqliteTransaction&>(*txn).note_mutation("lab", "l-find", test_ctx());
        txn->commit();
      }

      auto read_txn = backend.begin(storage::IsolationLevel::ReadCommitted);
      const auto events =
          read_txn->repo<core::AuditEvent>().query(storage::Query<core::AuditEvent>::all());
      ASSERT_EQ(events.size(), 1U);

      const auto found = read_txn->repo<core::AuditEvent>().find_by_id(events.front().id);
      ASSERT_TRUE(found.has_value());
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access): guarded by ASSERT_TRUE above
      EXPECT_EQ(found->entity_kind, "lab");
    }

    TEST(SqliteAuditEvent, AuditEventQuerySortByAt) {
      auto backend = make_audit_backend();

      {
        auto txn = backend.begin(storage::IsolationLevel::Serializable);
        dynamic_cast<storage::SqliteTransaction&>(*txn).note_mutation("lab", "l-a", test_ctx());
        txn->commit();
      }
      {
        auto txn = backend.begin(storage::IsolationLevel::Serializable);
        dynamic_cast<storage::SqliteTransaction&>(*txn).note_mutation("user", "u-b", test_ctx());
        txn->commit();
      }
      {
        auto txn = backend.begin(storage::IsolationLevel::Serializable);
        dynamic_cast<storage::SqliteTransaction&>(*txn).note_mutation("sample", "s-c", test_ctx());
        txn->commit();
      }

      auto read_txn = backend.begin(storage::IsolationLevel::ReadCommitted);

      const auto desc_query = storage::Query<core::AuditEvent>::all().order_by(
          storage::field<core::AuditEvent, std::int64_t>(core::AuditEvent::Field::At),
          storage::SortDirection::Descending);
      const auto desc_results = read_txn->repo<core::AuditEvent>().query(desc_query);
      ASSERT_EQ(desc_results.size(), 3U);
      EXPECT_GE(desc_results[0].at.unix_micros(), desc_results[1].at.unix_micros());
      EXPECT_GE(desc_results[1].at.unix_micros(), desc_results[2].at.unix_micros());

      const auto asc_query = storage::Query<core::AuditEvent>::all().order_by(
          storage::field<core::AuditEvent, std::int64_t>(core::AuditEvent::Field::At),
          storage::SortDirection::Ascending);
      const auto asc_results = read_txn->repo<core::AuditEvent>().query(asc_query);
      ASSERT_EQ(asc_results.size(), 3U);
      EXPECT_LE(asc_results[0].at.unix_micros(), asc_results[1].at.unix_micros());
      EXPECT_LE(asc_results[1].at.unix_micros(), asc_results[2].at.unix_micros());
    }

  } // namespace
} // namespace fmgr
