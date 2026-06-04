// SPDX-License-Identifier: AGPL-3.0-or-later

#include "storage/IStorageBackend.h"

#include "core/enums.h"
#include "core/ids.h"
#include "core/timestamp.h"

#include "test_helpers.h"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace fmgr::storage {
  namespace {
    using namespace fmgr::test;



    struct ConformanceSample {
      using Id = core::SampleId;

      enum class Field : std::uint8_t {
        Id,
        LabId,
        Name,
        Status,
        BoxId,
        PositionLabel,
        CustomFields,
        CreatedAt
      };

      Id id;
      core::LabId lab_id;
      std::string name;
      core::SampleStatus status{core::SampleStatus::Active};
      std::optional<core::BoxId> box_id;
      std::optional<std::string> position_label;
      nlohmann::json custom_fields = nlohmann::json::object();
      core::Timestamp created_at;

      friend bool operator==(const ConformanceSample&, const ConformanceSample&) = default;
    };

    struct UnsupportedConformanceEntity {
      using Id = core::BoxId;

      enum class Field : std::uint8_t { Id };
    };

  } // namespace

  template <> struct EntityTraits<ConformanceSample> {
    using Id = ConformanceSample::Id;
    using Field = ConformanceSample::Field;

    [[nodiscard]] static constexpr std::string_view entity_name() {
      return "conformance_sample";
    }

    [[nodiscard]] static constexpr Field tombstone_field() {
      return Field::Status;
    }
  };

  template <> struct EntityTraits<UnsupportedConformanceEntity> {
    using Id = UnsupportedConformanceEntity::Id;
    using Field = UnsupportedConformanceEntity::Field;

    [[nodiscard]] static constexpr std::string_view entity_name() {
      return "unsupported_conformance_entity";
    }

    [[nodiscard]] static constexpr Field tombstone_field() {
      return Field::Id;
    }
  };

  namespace {

    [[nodiscard]] MutationContext mutation_context();

    [[nodiscard]] ConformanceSample sample(std::uint64_t id_low_bits, std::string name,
                                           core::Timestamp created_at) {
      return ConformanceSample{
          .id = id_from_low<core::SampleId>(id_low_bits),
          .lab_id = id_from_low<core::LabId>(1),
          .name = std::move(name),
          .status = core::SampleStatus::Active,
          .box_id = id_from_low<core::BoxId>(100),
          .position_label = std::string("A") + std::to_string(id_low_bits),
          .custom_fields = nlohmann::json{{"project", "alpha"}},
          .created_at = created_at,
      };
    }

    struct StoredSample {
      ConformanceSample entity;
      std::uint64_t version{0};
    };

    struct InMemoryStorageState {
      std::map<core::SampleId, StoredSample> samples;
      SchemaVersion schema_version{0};
      std::uint64_t next_version{1};
      std::size_t audit_event_count{0};
      bool fail_next_audit_append{false};
      std::mutex mutex;
    };

    [[nodiscard]] bool is_active_occupant(core::SampleStatus status) {
      return status == core::SampleStatus::Active || status == core::SampleStatus::CheckedOut;
    }

    [[nodiscard]] std::optional<nlohmann::json> field_value(const ConformanceSample& entity,
                                                            ConformanceSample::Field field) {
      switch (field) {
      case ConformanceSample::Field::Id:
        return entity.id;
      case ConformanceSample::Field::LabId:
        return entity.lab_id;
      case ConformanceSample::Field::Name:
        return entity.name;
      case ConformanceSample::Field::Status:
        return entity.status;
      case ConformanceSample::Field::BoxId:
        if (entity.box_id.has_value()) {
          return entity.box_id.value();
        }
        return std::nullopt;
      case ConformanceSample::Field::PositionLabel:
        if (entity.position_label.has_value()) {
          return entity.position_label.value();
        }
        return std::nullopt;
      case ConformanceSample::Field::CustomFields:
        return entity.custom_fields;
      case ConformanceSample::Field::CreatedAt:
        return entity.created_at;
      }
      return std::nullopt;
    }

    [[nodiscard]] std::optional<nlohmann::json>
    json_path_value(const nlohmann::json& json, const std::vector<std::string>& path) {
      const nlohmann::json* current = &json;
      for (const auto& segment : path) {
        if (!current->is_object() || !current->contains(segment)) {
          return std::nullopt;
        }
        current = &current->at(segment);
      }
      return *current;
    }

    [[nodiscard]] bool json_less(const nlohmann::json& left, const nlohmann::json& right) {
      if (left.is_string() && right.is_string()) {
        return left.get<std::string>() < right.get<std::string>();
      }
      if (left.is_number_integer() && right.is_number_integer()) {
        return left.get<std::int64_t>() < right.get<std::int64_t>();
      }
      return left.dump() < right.dump();
    }

    [[nodiscard]] bool matches_predicate(const ConformanceSample& entity,
                                         const Predicate<ConformanceSample>& predicate) {
      if (predicate.op == PredicateOperator::JsonPathEqual) {
        const auto json_value = field_value(entity, predicate.field);
        if (!json_value.has_value()) {
          return false;
        }
        const auto path_value = json_path_value(json_value.value(), predicate.json_path);
        return path_value.has_value() && path_value.value() == predicate.value;
      }

      const auto value = field_value(entity, predicate.field);
      if (!value.has_value()) {
        return false;
      }

      switch (predicate.op) {
      case PredicateOperator::Equal:
        return value.value() == predicate.value;
      case PredicateOperator::GreaterThanOrEqual:
        return !json_less(value.value(), predicate.value);
      case PredicateOperator::LessThanOrEqual:
        return !json_less(predicate.value, value.value());
      case PredicateOperator::Between:
        return !json_less(value.value(), predicate.lower) &&
               !json_less(predicate.upper, value.value());
      case PredicateOperator::In:
        return std::ranges::any_of(predicate.values, [&](const nlohmann::json& candidate) {
          return value.value() == candidate;
        });
      case PredicateOperator::JsonPathEqual:
        return false;
      }
      return false;
    }

    [[nodiscard]] std::vector<ConformanceSample>
    apply_query(const std::map<core::SampleId, StoredSample>& samples,
                const Query<ConformanceSample>& query_spec) {
      std::vector<ConformanceSample> results;
      for (const auto& [unused_id, stored] : samples) {
        (void)unused_id;
        const auto& entity = stored.entity;
        if (!query_spec.includes_tombstoned() && entity.status == core::SampleStatus::Tombstoned) {
          continue;
        }
        if (std::ranges::all_of(query_spec.predicates(), [&](const auto& predicate) {
              return matches_predicate(entity, predicate);
            })) {
          results.push_back(entity);
        }
      }

      for (const auto sort_spec : query_spec.sorts() | std::views::reverse) {
        std::ranges::stable_sort(
            results, [&](const ConformanceSample& left, const ConformanceSample& right) {
              const auto left_value = field_value(left, sort_spec.field);
              const auto right_value = field_value(right, sort_spec.field);
              if (!left_value.has_value() || !right_value.has_value()) {
                return right_value.has_value();
              }
              const auto less = json_less(left_value.value(), right_value.value());
              if (sort_spec.direction == SortDirection::Ascending) {
                return less;
              }
              return json_less(right_value.value(), left_value.value());
            });
      }

      const auto offset = query_spec.offset_count().value_or(0);
      if (offset >= results.size()) {
        return {};
      }
      results.erase(results.begin(), results.begin() + static_cast<std::ptrdiff_t>(offset));

      if (query_spec.limit_count().has_value() &&
          query_spec.limit_count().value() < results.size()) {
        results.resize(query_spec.limit_count().value());
      }
      return results;
    }

    class InMemoryTransaction;

    class InMemorySampleRepository final : public IRepository<ConformanceSample> {
    public:
      explicit InMemorySampleRepository(InMemoryTransaction& transaction)
          : transaction_(transaction) {}

      [[nodiscard]] std::optional<ConformanceSample>
      find_by_id(const ConformanceSample::Id& entity_id) override;
      [[nodiscard]] std::vector<ConformanceSample>
      query(const Query<ConformanceSample>& query_spec) override;
      void insert(const ConformanceSample& entity, const MutationContext& context) override;
      void update(const ConformanceSample& entity, const MutationContext& context) override;
      void soft_delete(const ConformanceSample::Id& entity_id,
                       const MutationContext& context) override;

    private:
      InMemoryTransaction& transaction_;
    };

    class InMemoryTransaction final : public ITransaction {
    public:
      InMemoryTransaction(std::shared_ptr<InMemoryStorageState> state,
                          IsolationLevel isolation_level)
          : state_(std::move(state)), isolation_level_(isolation_level) {
        std::scoped_lock lock(state_->mutex);
        snapshot_ = state_->samples;
        working_ = snapshot_;
        register_repository<ConformanceSample>(std::make_unique<InMemorySampleRepository>(*this));
      }

      void commit() override {
        std::scoped_lock lock(state_->mutex);
        if (completed_) {
          throw ConstraintViolation("transaction already completed");
        }

        if (state_->fail_next_audit_append && mutation_count_ > 0) {
          state_->fail_next_audit_append = false;
          throw ConstraintViolation("audit append failed");
        }

        if (isolation_level_ == IsolationLevel::Serializable) {
          for (const auto& entity_id : write_set_) {
            const auto current = state_->samples.find(entity_id);
            const auto original = snapshot_.find(entity_id);
            const auto current_version =
                current == state_->samples.end()
                    ? std::optional<std::uint64_t>{}
                    : std::optional<std::uint64_t>{current->second.version};
            const auto original_version =
                original == snapshot_.end()
                    ? std::optional<std::uint64_t>{}
                    : std::optional<std::uint64_t>{original->second.version};
            if (current_version != original_version) {
              throw SerializationFailure("serializable transaction conflict");
            }
          }
        }

        auto merged = state_->samples;
        for (const auto& entity_id : write_set_) {
          merged.insert_or_assign(entity_id, working_.at(entity_id));
        }
        validate_active_positions(merged);

        for (auto& [entity_id, stored] : merged) {
          const auto original = snapshot_.find(entity_id);
          if (write_set_.contains(entity_id) ||
              (original != snapshot_.end() && original->second.entity != stored.entity)) {
            stored.version = state_->next_version++;
          }
        }

        state_->samples = std::move(merged);
        state_->audit_event_count += mutation_count_;
        completed_ = true;
      }

      void rollback() override {
        completed_ = true;
      }

      [[nodiscard]] std::optional<ConformanceSample>
      find_sample(const ConformanceSample::Id& entity_id) const {
        const auto iterator = working_.find(entity_id);
        if (iterator == working_.end()) {
          return std::nullopt;
        }
        return iterator->second.entity;
      }

      [[nodiscard]] std::vector<ConformanceSample>
      query_samples(const Query<ConformanceSample>& query_spec) const {
        return apply_query(working_, query_spec);
      }

      void insert_sample(const ConformanceSample& entity) {
        if (working_.contains(entity.id)) {
          throw UniqueViolation("sample id already exists");
        }
        auto next = working_;
        next.emplace(entity.id, StoredSample{.entity = entity});
        validate_active_positions(next);
        working_ = std::move(next);
        write_set_.insert(entity.id);
        ++mutation_count_;
      }

      void update_sample(const ConformanceSample& entity) {
        const auto iterator = working_.find(entity.id);
        if (iterator == working_.end()) {
          throw NotFound("sample not found");
        }
        auto next = working_;
        next.at(entity.id).entity = entity;
        validate_active_positions(next);
        working_ = std::move(next);
        write_set_.insert(entity.id);
        ++mutation_count_;
      }

      void soft_delete_sample(const ConformanceSample::Id& entity_id) {
        auto iterator = working_.find(entity_id);
        if (iterator == working_.end()) {
          throw NotFound("sample not found");
        }
        iterator->second.entity.status = core::SampleStatus::Tombstoned;
        write_set_.insert(entity_id);
        ++mutation_count_;
      }

    private:
      static void validate_active_positions(const std::map<core::SampleId, StoredSample>& samples) {
        std::set<std::pair<core::BoxId, std::string>> occupied;
        for (const auto& [unused_id, stored] : samples) {
          (void)unused_id;
          const auto& entity = stored.entity;
          if (!is_active_occupant(entity.status) || !entity.box_id.has_value() ||
              !entity.position_label.has_value()) {
            continue;
          }
          const auto position =
              std::make_pair(entity.box_id.value(), entity.position_label.value());
          if (!occupied.insert(position).second) {
            throw UniqueViolation("active box position is already occupied");
          }
        }
      }

      std::shared_ptr<InMemoryStorageState> state_;
      IsolationLevel isolation_level_;
      std::map<core::SampleId, StoredSample> snapshot_;
      std::map<core::SampleId, StoredSample> working_;
      std::set<core::SampleId> write_set_;
      std::size_t mutation_count_{0};
      bool completed_{false};
    };

    std::optional<ConformanceSample>
    InMemorySampleRepository::find_by_id(const ConformanceSample::Id& entity_id) {
      return transaction_.find_sample(entity_id);
    }

    std::vector<ConformanceSample>
    InMemorySampleRepository::query(const Query<ConformanceSample>& query_spec) {
      return transaction_.query_samples(query_spec);
    }

    void InMemorySampleRepository::insert(const ConformanceSample& entity,
                                          const MutationContext& context) {
      (void)context;
      transaction_.insert_sample(entity);
    }

    void InMemorySampleRepository::update(const ConformanceSample& entity,
                                          const MutationContext& context) {
      (void)context;
      transaction_.update_sample(entity);
    }

    void InMemorySampleRepository::soft_delete(const ConformanceSample::Id& entity_id,
                                               const MutationContext& context) {
      (void)context;
      transaction_.soft_delete_sample(entity_id);
    }

    class InMemoryStorageBackend final : public IStorageBackend {
    public:
      explicit InMemoryStorageBackend(std::shared_ptr<InMemoryStorageState> state)
          : state_(std::move(state)) {}

      void migrate_to_latest() override {
        std::scoped_lock lock(state_->mutex);
        state_->schema_version = SchemaVersion{1};
      }

      [[nodiscard]] SchemaVersion current_version() const override {
        std::scoped_lock lock(state_->mutex);
        return state_->schema_version;
      }

      [[nodiscard]] std::unique_ptr<ITransaction> begin(IsolationLevel isolation_level) override {
        return std::make_unique<InMemoryTransaction>(state_, isolation_level);
      }

      [[nodiscard]] Capabilities caps() const override {
        Capabilities capabilities;
        capabilities.json_path_equality = true;
        capabilities.native_uuid = false;
        return capabilities;
      }

    private:
      std::shared_ptr<InMemoryStorageState> state_;
    };

    class StorageConformanceDriver {
    public:
      virtual ~StorageConformanceDriver() = default;

      virtual void reset() = 0;
      [[nodiscard]] virtual IStorageBackend& backend() = 0;
      [[nodiscard]] virtual std::size_t audit_event_count() const = 0;
      virtual void fail_next_audit_append() = 0;
      virtual void downgrade_to_zero() = 0;
      virtual void seed_representative_data() = 0;
      [[nodiscard]] virtual bool has_representative_data() const = 0;
    };

    class InMemoryConformanceDriver final : public StorageConformanceDriver {
    public:
      InMemoryConformanceDriver()
          : state_(std::make_shared<InMemoryStorageState>()), backend_(state_) {}

      void reset() override {
        std::scoped_lock lock(state_->mutex);
        state_->samples.clear();
        state_->schema_version = SchemaVersion{0};
        state_->next_version = 1;
        state_->audit_event_count = 0;
        state_->fail_next_audit_append = false;
      }

      [[nodiscard]] IStorageBackend& backend() override {
        return backend_;
      }

      [[nodiscard]] std::size_t audit_event_count() const override {
        std::scoped_lock lock(state_->mutex);
        return state_->audit_event_count;
      }

      void fail_next_audit_append() override {
        std::scoped_lock lock(state_->mutex);
        state_->fail_next_audit_append = true;
      }

      void downgrade_to_zero() override {
        std::scoped_lock lock(state_->mutex);
        state_->schema_version = SchemaVersion{0};
      }

      void seed_representative_data() override {
        auto transaction = backend_.begin(IsolationLevel::Serializable);
        auto& repository = transaction->repo<ConformanceSample>();
        repository.insert(sample(900, "seed", core::Timestamp::from_unix_micros(900)),
                          mutation_context());
        transaction->commit();
      }

      [[nodiscard]] bool has_representative_data() const override {
        std::scoped_lock lock(state_->mutex);
        return state_->samples.contains(id_from_low<core::SampleId>(900));
      }

    private:
      std::shared_ptr<InMemoryStorageState> state_;
      InMemoryStorageBackend backend_;
    };

    [[nodiscard]] MutationContext mutation_context() {
      return MutationContext{
          .actor_user_id = id_from_low<core::UserId>(500),
          .actor_session_id = "conformance-session",
          .request_id = "conformance-request",
          .reason = "backend conformance test",
          .lab_id = id_from_low<core::LabId>(1).to_string(),
      };
    }

    class BackendConformanceTest : public ::testing::Test {
    protected:
      void SetUp() override {
        driver_.reset();
        driver_.backend().migrate_to_latest();
      }

      [[nodiscard]] StorageConformanceDriver& driver() {
        return driver_;
      }

    private:
      InMemoryConformanceDriver driver_;
    };

    TEST_F(BackendConformanceTest, CrudRoundTripUpdatesAndSoftDeletesSample) {
      auto transaction = driver().backend().begin(IsolationLevel::Serializable);
      auto& repository = transaction->repo<ConformanceSample>();
      auto entity = sample(1, "alpha", core::Timestamp::from_unix_micros(100));

      repository.insert(entity, mutation_context());
      transaction->commit();

      transaction = driver().backend().begin(IsolationLevel::Serializable);
      auto& read_repository = transaction->repo<ConformanceSample>();
      auto stored = read_repository.find_by_id(entity.id);
      ASSERT_TRUE(stored.has_value());
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
      auto stored_value = stored.value();
      EXPECT_EQ(stored_value.name, "alpha");

      entity.name = "beta";
      read_repository.update(entity, mutation_context());
      read_repository.soft_delete(entity.id, mutation_context());
      transaction->commit();

      transaction = driver().backend().begin(IsolationLevel::Serializable);
      auto& final_repository = transaction->repo<ConformanceSample>();
      stored = final_repository.find_by_id(entity.id);
      ASSERT_TRUE(stored.has_value());
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
      stored_value = stored.value();
      EXPECT_EQ(stored_value.name, "beta");
      EXPECT_EQ(stored_value.status, core::SampleStatus::Tombstoned);
    }

    TEST_F(BackendConformanceTest, QueryDslAppliesFiltersSortingAndPagination) {
      auto transaction = driver().backend().begin(IsolationLevel::Serializable);
      auto& repository = transaction->repo<ConformanceSample>();

      auto alpha = sample(10, "alpha", core::Timestamp::from_unix_micros(100));
      auto beta = sample(11, "beta", core::Timestamp::from_unix_micros(200));
      auto gamma = sample(12, "gamma", core::Timestamp::from_unix_micros(300));
      beta.custom_fields = nlohmann::json{{"project", "beta-project"}};
      gamma.custom_fields = nlohmann::json{{"project", "beta-project"}};

      repository.insert(alpha, mutation_context());
      repository.insert(beta, mutation_context());
      repository.insert(gamma, mutation_context());
      transaction->commit();

      transaction = driver().backend().begin(IsolationLevel::Serializable);
      auto& query_repository = transaction->repo<ConformanceSample>();
      const auto query =
          Query<ConformanceSample>::where(
              field<ConformanceSample, core::LabId>(ConformanceSample::Field::LabId) ==
              id_from_low<core::LabId>(1))
              .and_where(
                  field<ConformanceSample, core::Timestamp>(ConformanceSample::Field::CreatedAt)
                      .between(core::Timestamp::from_unix_micros(100),
                               core::Timestamp::from_unix_micros(300)))
              .and_where(field<ConformanceSample, std::string>(ConformanceSample::Field::Name)
                             .in({"beta", "gamma"}))
              .and_where(json_path<ConformanceSample>(ConformanceSample::Field::CustomFields,
                                                      {"project"}) == "beta-project")
              .order_by(field<ConformanceSample, std::string>(ConformanceSample::Field::Name),
                        SortDirection::Descending)
              .limit(1)
              .offset(1);

      const auto results = query_repository.query(query);

      ASSERT_EQ(results.size(), 1U);
      EXPECT_EQ(results.front().name, "beta");
    }

    TEST_F(BackendConformanceTest, SoftDeletedRowsAreHiddenUnlessIncluded) {
      auto entity = sample(20, "hidden", core::Timestamp::from_unix_micros(100));
      auto transaction = driver().backend().begin(IsolationLevel::Serializable);
      auto& repository = transaction->repo<ConformanceSample>();
      repository.insert(entity, mutation_context());
      repository.soft_delete(entity.id, mutation_context());
      transaction->commit();

      transaction = driver().backend().begin(IsolationLevel::Serializable);
      auto& query_repository = transaction->repo<ConformanceSample>();
      EXPECT_TRUE(query_repository.query(Query<ConformanceSample>::all()).empty());

      const auto visible =
          query_repository.query(Query<ConformanceSample>::all().include_tombstoned());
      ASSERT_EQ(visible.size(), 1U);
      EXPECT_EQ(visible.front().id, entity.id);
    }

    TEST_F(BackendConformanceTest, DuplicateActiveBoxPositionThrowsPortableUniqueViolation) {
      auto left = sample(30, "left", core::Timestamp::from_unix_micros(100));
      auto right = sample(31, "right", core::Timestamp::from_unix_micros(101));
      right.box_id = left.box_id;
      right.position_label = left.position_label;

      auto transaction = driver().backend().begin(IsolationLevel::Serializable);
      auto& repository = transaction->repo<ConformanceSample>();
      repository.insert(left, mutation_context());

      EXPECT_THROW(repository.insert(right, mutation_context()), UniqueViolation);
    }

    TEST_F(BackendConformanceTest, UnsupportedEntityRepositoryThrowsPortableError) {
      auto transaction = driver().backend().begin(IsolationLevel::Serializable);
      EXPECT_THROW((void)transaction->repo<UnsupportedConformanceEntity>(), UnsupportedOperation);
    }

    TEST_F(BackendConformanceTest, SerializableOverlappingUpdatesRejectOneCommit) {
      auto entity = sample(40, "original", core::Timestamp::from_unix_micros(100));
      auto seed_transaction = driver().backend().begin(IsolationLevel::Serializable);
      seed_transaction->repo<ConformanceSample>().insert(entity, mutation_context());
      seed_transaction->commit();

      auto left_transaction = driver().backend().begin(IsolationLevel::Serializable);
      auto right_transaction = driver().backend().begin(IsolationLevel::Serializable);

      auto& left_repository = left_transaction->repo<ConformanceSample>();
      auto& right_repository = right_transaction->repo<ConformanceSample>();

      auto left = left_repository.find_by_id(entity.id);
      auto right = right_repository.find_by_id(entity.id);
      ASSERT_TRUE(left.has_value());
      ASSERT_TRUE(right.has_value());
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
      auto left_entity = left.value();
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
      auto right_entity = right.value();
      left_entity.name = "left";
      right_entity.name = "right";
      left_repository.update(left_entity, mutation_context());
      right_repository.update(right_entity, mutation_context());

      left_transaction->commit();
      EXPECT_THROW(right_transaction->commit(), SerializationFailure);
    }

    TEST_F(BackendConformanceTest, ConcurrentPlacementPreservesActivePositionUniqueness) {
      const auto stress = std::getenv("FMGR_STORAGE_STRESS") != nullptr;
      const std::size_t thread_count = stress ? 50U : 8U;
      const std::size_t attempts_per_thread = stress ? 1000U : 20U;

      std::mutex result_mutex;
      std::size_t successes = 0;
      std::size_t unique_violations = 0;
      std::vector<std::thread> threads;
      threads.reserve(thread_count);

      for (std::size_t thread_index = 0; thread_index < thread_count; ++thread_index) {
        threads.emplace_back([&, thread_index]() {
          for (std::size_t attempt = 0; attempt < attempts_per_thread; ++attempt) {
            auto entity = sample(1000 + (thread_index * attempts_per_thread) + attempt, "placed",
                                 core::Timestamp::from_unix_micros(100));
            entity.box_id = id_from_low<core::BoxId>(777);
            entity.position_label = "A1";

            try {
              auto transaction = driver().backend().begin(IsolationLevel::Serializable);
              auto& repository = transaction->repo<ConformanceSample>();
              repository.insert(entity, mutation_context());
              transaction->commit();
              std::scoped_lock lock(result_mutex);
              ++successes;
            } catch (const UniqueViolation&) {
              std::scoped_lock lock(result_mutex);
              ++unique_violations;
            } catch (const SerializationFailure&) {
              std::scoped_lock lock(result_mutex);
              ++unique_violations;
            }
          }
        });
      }

      for (auto& thread : threads) {
        thread.join();
      }

      EXPECT_EQ(successes, 1U);
      EXPECT_EQ(unique_violations, (thread_count * attempts_per_thread) - 1U);
    }

    TEST_F(BackendConformanceTest, MutationsAppendAuditEventsAtomically) {
      auto entity = sample(50, "audited", core::Timestamp::from_unix_micros(100));
      auto transaction = driver().backend().begin(IsolationLevel::Serializable);
      transaction->repo<ConformanceSample>().insert(entity, mutation_context());
      transaction->commit();

      EXPECT_EQ(driver().audit_event_count(), 1U);
    }

    TEST_F(BackendConformanceTest, AuditAppendFailurePreventsMutationCommit) {
      auto entity = sample(60, "audit-failure", core::Timestamp::from_unix_micros(100));
      auto transaction = driver().backend().begin(IsolationLevel::Serializable);
      transaction->repo<ConformanceSample>().insert(entity, mutation_context());
      driver().fail_next_audit_append();

      EXPECT_THROW(transaction->commit(), ConstraintViolation);

      transaction = driver().backend().begin(IsolationLevel::Serializable);
      EXPECT_FALSE(transaction->repo<ConformanceSample>().find_by_id(entity.id).has_value());
      EXPECT_EQ(driver().audit_event_count(), 0U);
    }

    TEST_F(BackendConformanceTest, MigrationCanDowngradeAndForwardMigrateSeedData) {
      driver().seed_representative_data();
      ASSERT_TRUE(driver().has_representative_data());

      driver().downgrade_to_zero();
      EXPECT_EQ(driver().backend().current_version(), SchemaVersion{0});

      driver().backend().migrate_to_latest();
      EXPECT_EQ(driver().backend().current_version(), SchemaVersion{1});
      EXPECT_TRUE(driver().has_representative_data());
    }

  } // namespace
} // namespace fmgr::storage
