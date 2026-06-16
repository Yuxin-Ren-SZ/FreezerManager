// SPDX-License-Identifier: AGPL-3.0-or-later

#include "storage/IStorageBackend.h"

#include "core/ids.h"
#include "core/timestamp.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fmgr::storage {
  namespace {

    struct TestSample {
      using Id = core::SampleId;

      enum class Field : std::uint8_t { Id, LabId, Name, Status, CreatedAt, CustomFields };

      Id id;
      core::LabId lab_id;
      std::string name;
      core::Timestamp created_at;
    };

    struct UnsupportedEntity {
      using Id = core::BoxId;

      enum class Field : std::uint8_t { Id };
    };

  } // namespace

  template <> struct EntityTraits<TestSample> {
    using Id = TestSample::Id;
    using Field = TestSample::Field;

    [[nodiscard]] static constexpr std::string_view entity_name() {
      return "test_sample";
    }

    [[nodiscard]] static constexpr Field tombstone_field() {
      return Field::Status;
    }
  };

  template <> struct EntityTraits<UnsupportedEntity> {
    using Id = UnsupportedEntity::Id;
    using Field = UnsupportedEntity::Field;

    [[nodiscard]] static constexpr std::string_view entity_name() {
      return "unsupported_entity";
    }

    [[nodiscard]] static constexpr Field tombstone_field() {
      return Field::Id;
    }
  };

  namespace {

    class TestSampleRepository final : public IRepository<TestSample> {
    public:
      std::optional<TestSample> find_by_id(const TestSample::Id& entity_id) override {
        last_find_id = entity_id;
        return std::nullopt;
      }

      std::vector<TestSample> query(const Query<TestSample>& query_spec) override {
        last_query = query_spec;
        return {};
      }

      void insert(const TestSample& entity, const MutationContext& context) override {
        inserted = entity;
        mutation_context = context;
      }

      void update(const TestSample& entity, const MutationContext& context) override {
        updated = entity;
        mutation_context = context;
      }

      void soft_delete(const TestSample::Id& entity_id, const MutationContext& context) override {
        soft_deleted_id = entity_id;
        mutation_context = context;
      }

      std::optional<TestSample::Id> last_find_id;
      std::optional<Query<TestSample>> last_query;
      std::optional<TestSample> inserted;
      std::optional<TestSample> updated;
      std::optional<TestSample::Id> soft_deleted_id;
      std::optional<MutationContext> mutation_context;
    };

    class TestTransaction final : public ITransaction {
    public:
      TestTransaction() {
        register_repository<TestSample>(std::make_unique<TestSampleRepository>());
      }

      void commit() override {
        committed = true;
      }

      void rollback() override {
        rolled_back = true;
      }

      bool committed{false};
      bool rolled_back{false};
    };

    class TestBackend final : public IStorageBackend {
    public:
      void migrate_to_latest() override {
        migrated = true;
      }

      [[nodiscard]] SchemaVersion current_version() const override {
        return SchemaVersion{7};
      }

      std::unique_ptr<ITransaction> begin(IsolationLevel isolation_level) override {
        last_isolation_level = isolation_level;
        return std::make_unique<TestTransaction>();
      }

      [[nodiscard]] Capabilities caps() const override {
        Capabilities capabilities;
        capabilities.row_level_security = true;
        capabilities.json_path_equality = true;
        capabilities.native_uuid = true;
        capabilities.listen_notify = true;
        return capabilities;
      }

      bool migrated{false};
      std::optional<IsolationLevel> last_isolation_level;
    };

    TEST(StorageInterface, SchemaVersionComparesByVersionNumber) {
      EXPECT_LT(SchemaVersion{1}, SchemaVersion{2});
      EXPECT_EQ(SchemaVersion{3}, SchemaVersion{3});
    }

    TEST(StorageInterface, CapabilitiesDefaultToPortableSubset) {
      const Capabilities capabilities;

      EXPECT_FALSE(capabilities.row_level_security);
      EXPECT_FALSE(capabilities.json_path_equality);
      EXPECT_FALSE(capabilities.json_path_indexes);
      EXPECT_FALSE(capabilities.native_uuid);
      EXPECT_FALSE(capabilities.listen_notify);
    }

    TEST(StorageInterface, BackendErrorPreservesPortableCodeAndMessage) {
      const UniqueViolation error("duplicate box position");

      EXPECT_EQ(error.code(), BackendErrorCode::UniqueViolation);
      EXPECT_STREQ(error.what(), "duplicate box position");
      EXPECT_THROW(throw SerializationFailure("retry transaction"), BackendError);
    }

    TEST(StorageInterface, QueryBuilderRecordsPortableTypedClauses) {
      const auto lab_id = core::LabId::parse("550e8400-e29b-41d4-a716-446655440000");
      const auto oldest = core::Timestamp::from_unix_micros(100);
      const auto newest = core::Timestamp::from_unix_micros(200);

      const auto query =
          Query<TestSample>::where(field<TestSample, core::LabId>(TestSample::Field::LabId) ==
                                   lab_id)
              .and_where(field<TestSample, core::Timestamp>(TestSample::Field::CreatedAt)
                             .between(oldest, newest))
              .and_where(
                  field<TestSample, std::string>(TestSample::Field::Name).in({"alpha", "beta"}))
              .and_where(json_path<TestSample>(TestSample::Field::CustomFields, {"project"}) ==
                         "oncology")
              .order_by(field<TestSample, std::string>(TestSample::Field::Name),
                        SortDirection::Ascending)
              .limit(50)
              .offset(100);

      ASSERT_EQ(query.predicates().size(), 4U);
      EXPECT_EQ(query.predicates().at(0).op, PredicateOperator::Equal);
      EXPECT_EQ(query.predicates().at(0).field, TestSample::Field::LabId);
      EXPECT_EQ(query.predicates().at(0).value.get<core::LabId>(), lab_id);

      EXPECT_EQ(query.predicates().at(1).op, PredicateOperator::Between);
      EXPECT_EQ(query.predicates().at(1).lower.get<core::Timestamp>(), oldest);
      EXPECT_EQ(query.predicates().at(1).upper.get<core::Timestamp>(), newest);

      EXPECT_EQ(query.predicates().at(2).op, PredicateOperator::In);
      EXPECT_EQ(query.predicates().at(2).values.at(0).get<std::string>(), "alpha");
      EXPECT_EQ(query.predicates().at(2).values.at(1).get<std::string>(), "beta");

      EXPECT_EQ(query.predicates().at(3).op, PredicateOperator::JsonPathEqual);
      EXPECT_EQ(query.predicates().at(3).json_path, std::vector<std::string>({"project"}));
      EXPECT_EQ(query.predicates().at(3).value.get<std::string>(), "oncology");

      ASSERT_EQ(query.sorts().size(), 1U);
      EXPECT_EQ(query.sorts().at(0).field, TestSample::Field::Name);
      EXPECT_EQ(query.sorts().at(0).direction, SortDirection::Ascending);
      EXPECT_EQ(query.limit_count(), 50U);
      EXPECT_EQ(query.offset_count(), 100U);
      EXPECT_FALSE(query.includes_tombstoned());
    }

    TEST(StorageInterface, QueryCanOptIntoTombstonedRows) {
      const auto query = Query<TestSample>::all().include_tombstoned();

      EXPECT_TRUE(query.includes_tombstoned());
    }

    TEST(StorageInterface, TransactionReturnsTypedRepositories) {
      TestTransaction transaction;
      auto& repository = transaction.repo<TestSample>();

      const auto sample_id = core::SampleId::parse("550e8400-e29b-41d4-a716-446655440000");
      EXPECT_EQ(repository.find_by_id(sample_id), std::nullopt);

      const MutationContext context{
          .actor_user_id = core::UserId::parse("650e8400-e29b-41d4-a716-446655440000"),
          .actor_session_id = "session-1",
          .request_id = "request-1",
          .reason = "unit test",
      };
      repository.soft_delete(sample_id, context);

      auto& concrete = dynamic_cast<TestSampleRepository&>(repository);
      EXPECT_EQ(concrete.last_find_id, sample_id);
      EXPECT_EQ(concrete.soft_deleted_id, sample_id);
      ASSERT_TRUE(concrete.mutation_context.has_value());
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
      const auto mutation_context = concrete.mutation_context.value();
      EXPECT_EQ(mutation_context.actor_session_id, "session-1");

      EXPECT_THROW((void)transaction.repo<UnsupportedEntity>(), UnsupportedOperation);
    }

    TEST(StorageInterface, BackendExposesLifecycleTransactionsAndCapabilities) {
      TestBackend backend;

      backend.migrate_to_latest();
      auto transaction = backend.begin(IsolationLevel::Serializable);
      transaction->commit();

      EXPECT_TRUE(backend.migrated);
      EXPECT_EQ(backend.current_version(), SchemaVersion{7});
      EXPECT_EQ(backend.last_isolation_level, IsolationLevel::Serializable);
      EXPECT_TRUE(backend.caps().row_level_security);
      EXPECT_TRUE(backend.caps().json_path_equality);
      EXPECT_TRUE(backend.caps().native_uuid);
      EXPECT_TRUE(backend.caps().listen_notify);
    }

    // ---- Break-it: query builder edge cases ----

    TEST(StorageInterface, QueryWithContradictoryPredicatesAcceptedByBuilder) {
      // The Query builder is a passive data structure — stored backends resolve
      // contradictions. The builder must accept them without crash.
      const auto query =
          Query<TestSample>::where(
              field<TestSample, std::string>(TestSample::Field::Name) == "alpha")
              .and_where(
                  field<TestSample, std::string>(TestSample::Field::Name) == "beta");
      EXPECT_EQ(query.predicates().size(), 2U);
    }

    TEST(StorageInterface, QueryWithZeroLimit) {
      const auto query = Query<TestSample>::all().limit(0);
      EXPECT_EQ(query.limit_count(), 0U);
    }

    TEST(StorageInterface, QueryWithLargeLimit) {
      const auto query = Query<TestSample>::all().limit(1'000'000);
      EXPECT_EQ(query.limit_count(), 1'000'000U);
    }

    TEST(StorageInterface, QueryOffsetWithoutLimit) {
      const auto query = Query<TestSample>::all().offset(50);
      EXPECT_EQ(query.offset_count(), 50U);
      EXPECT_FALSE(query.limit_count().has_value());
    }

    TEST(StorageInterface, QueryTombstonedThenNotIsIdempotent) {
      // include_tombstoned() is a flag, not a toggle — calling it multiple times
      // should still result in true.
      auto query = Query<TestSample>::all().include_tombstoned().include_tombstoned();
      EXPECT_TRUE(query.includes_tombstoned());
    }

    TEST(StorageInterface, QueryWithInOperatorEmptyList) {
      const auto query = Query<TestSample>::where(
          field<TestSample, std::string>(TestSample::Field::Name)
              .in(std::vector<std::string>{}));
      ASSERT_EQ(query.predicates().size(), 1U);
      EXPECT_EQ(query.predicates().at(0).op, PredicateOperator::In);
      EXPECT_TRUE(query.predicates().at(0).values.empty());
    }

    TEST(StorageInterface, QueryChainingMultipleSorts) {
      const auto query =
          Query<TestSample>::all()
              .order_by(field<TestSample, std::string>(TestSample::Field::Name),
                        SortDirection::Ascending)
              .order_by(field<TestSample, core::Timestamp>(TestSample::Field::CreatedAt),
                        SortDirection::Descending);
      EXPECT_EQ(query.sorts().size(), 2U);
      EXPECT_EQ(query.sorts().at(0).direction, SortDirection::Ascending);
      EXPECT_EQ(query.sorts().at(1).direction, SortDirection::Descending);
    }

    TEST(StorageInterface, QueryDefaultsExcludeTombstoned) {
      const auto query = Query<TestSample>::all();
      EXPECT_FALSE(query.includes_tombstoned());
      EXPECT_EQ(query.predicates().size(), 0U);
      EXPECT_EQ(query.sorts().size(), 0U);
    }

    TEST(StorageInterface, MutationContextDefaultIsEmpty) {
      const MutationContext ctx{};
      EXPECT_TRUE(ctx.actor_session_id.empty());
      EXPECT_TRUE(ctx.request_id.empty());
      EXPECT_TRUE(ctx.reason.empty());
    }

    // ==== Aggressive: edge-of-representation query constructs ====

    TEST(StorageInterface, BetweenPredicateWithEqualBounds) {
      const auto t = core::Timestamp::from_unix_micros(100);
      const auto query = Query<TestSample>::where(
          field<TestSample, core::Timestamp>(TestSample::Field::CreatedAt).between(t, t));
      ASSERT_EQ(query.predicates().size(), 1U);
      EXPECT_EQ(query.predicates().at(0).op, PredicateOperator::Between);
      EXPECT_EQ(query.predicates().at(0).lower.get<core::Timestamp>(), t);
      EXPECT_EQ(query.predicates().at(0).upper.get<core::Timestamp>(), t);
    }

    TEST(StorageInterface, BetweenPredicateWithReversedBounds) {
      const auto early = core::Timestamp::from_unix_micros(100);
      const auto late = core::Timestamp::from_unix_micros(200);
      const auto query = Query<TestSample>::where(
          field<TestSample, core::Timestamp>(TestSample::Field::CreatedAt).between(late, early));
      ASSERT_EQ(query.predicates().size(), 1U);
    }

    TEST(StorageInterface, JsonPathWithEmptySegmentList) {
      const auto query = Query<TestSample>::where(
          json_path<TestSample>(TestSample::Field::CustomFields, {}) == "value");
      ASSERT_EQ(query.predicates().size(), 1U);
      EXPECT_EQ(query.predicates().at(0).op, PredicateOperator::JsonPathEqual);
      EXPECT_TRUE(query.predicates().at(0).json_path.empty());
    }

    TEST(StorageInterface, QueryWithOnlySortsNoPredicates) {
      const auto query =
          Query<TestSample>::all()
              .order_by(field<TestSample, std::string>(TestSample::Field::Name));
      EXPECT_EQ(query.predicates().size(), 0U);
      EXPECT_EQ(query.sorts().size(), 1U);
    }

    TEST(StorageInterface, BetweenPredicateOnStringField) {
      const auto query = Query<TestSample>::where(
          field<TestSample, std::string>(TestSample::Field::Name).between("a", "z"));
      ASSERT_EQ(query.predicates().size(), 1U);
      EXPECT_EQ(query.predicates().at(0).op, PredicateOperator::Between);
    }

  } // namespace
} // namespace fmgr::storage
