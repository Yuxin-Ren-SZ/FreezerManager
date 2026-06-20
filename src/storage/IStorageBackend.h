// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_STORAGE_ISTORAGEBACKEND_H
#define FMGR_STORAGE_ISTORAGEBACKEND_H

#include "core/ids.h"

#include <nlohmann/json.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fmgr::storage {

  struct SchemaVersion {
    int value{0};

    friend constexpr auto operator<=>(const SchemaVersion&, const SchemaVersion&) = default;
  };

  enum class IsolationLevel : std::uint8_t { ReadCommitted, RepeatableRead, Serializable };

  struct Capabilities {
    bool row_level_security{false};
    bool json_path_equality{false};
    bool json_path_indexes{false};
    bool native_uuid{false};
    bool listen_notify{false};
  };

  struct MutationContext {
    core::UserId actor_user_id;
    std::string actor_session_id;
    std::string request_id;
    std::string reason;
    // Lab that owns the mutated entity. Null for cross-lab entities (sessions, audit).
    std::optional<std::string> lab_id;
    // NOTE: before/after snapshots are intentionally NOT carried here. The
    // repository derives them from authoritative entity state and passes them to
    // ITransaction::note_mutation, so a caller cannot forge audit content.
  };

  // Repository-derived before/after entity snapshots for one audited mutation.
  // The repository owns these (it has the authoritative state); callers never
  // supply them. `before` is null for inserts; `after` is null for hard deletes.
  struct AuditSnapshot {
    std::optional<nlohmann::json> before;
    std::optional<nlohmann::json> after;
  };

  enum class BackendErrorCode : std::uint8_t {
    UniqueViolation,
    ForeignKeyViolation,
    SerializationFailure,
    Unavailable,
    NotFound,
    ConstraintViolation,
    MigrationFailure,
    UnsupportedOperation,
  };

  class BackendError : public std::runtime_error {
  public:
    BackendError(BackendErrorCode code, std::string_view message)
        : std::runtime_error(std::string(message)), code_(code) {}

    [[nodiscard]] constexpr BackendErrorCode code() const noexcept {
      return code_;
    }

  private:
    BackendErrorCode code_;
  };

  class UniqueViolation final : public BackendError {
  public:
    explicit UniqueViolation(std::string_view message)
        : BackendError(BackendErrorCode::UniqueViolation, message) {}
  };

  class ForeignKeyViolation final : public BackendError {
  public:
    explicit ForeignKeyViolation(std::string_view message)
        : BackendError(BackendErrorCode::ForeignKeyViolation, message) {}
  };

  class SerializationFailure final : public BackendError {
  public:
    explicit SerializationFailure(std::string_view message)
        : BackendError(BackendErrorCode::SerializationFailure, message) {}
  };

  class Unavailable final : public BackendError {
  public:
    explicit Unavailable(std::string_view message)
        : BackendError(BackendErrorCode::Unavailable, message) {}
  };

  class NotFound final : public BackendError {
  public:
    explicit NotFound(std::string_view message)
        : BackendError(BackendErrorCode::NotFound, message) {}
  };

  class ConstraintViolation final : public BackendError {
  public:
    explicit ConstraintViolation(std::string_view message)
        : BackendError(BackendErrorCode::ConstraintViolation, message) {}
  };

  class MigrationFailure final : public BackendError {
  public:
    explicit MigrationFailure(std::string_view message)
        : BackendError(BackendErrorCode::MigrationFailure, message) {}
  };

  class UnsupportedOperation final : public BackendError {
  public:
    explicit UnsupportedOperation(std::string_view message)
        : BackendError(BackendErrorCode::UnsupportedOperation, message) {}
  };

  template <typename Entity> struct EntityTraits;

  enum class PredicateOperator : std::uint8_t {
    Equal,
    GreaterThanOrEqual,
    LessThanOrEqual,
    Between,
    In,
    JsonPathEqual
  };

  enum class SortDirection : std::uint8_t { Ascending, Descending };

  template <typename Entity> struct Predicate;

  template <typename Entity, typename Value> class FieldRef {
  public:
    using Field = typename EntityTraits<Entity>::Field;

    explicit constexpr FieldRef(Field field) : field_(field) {}

    [[nodiscard]] constexpr Field field() const {
      return field_;
    }

    [[nodiscard]] Predicate<Entity> between(const Value& lower, const Value& upper) const;
    [[nodiscard]] Predicate<Entity> in(const std::vector<Value>& values) const;

  private:
    Field field_;
  };

  template <typename Entity> class JsonPathRef {
  public:
    using Field = typename EntityTraits<Entity>::Field;

    JsonPathRef(Field field, std::vector<std::string> path)
        : field_(field), path_(std::move(path)) {}

    [[nodiscard]] constexpr Field field() const {
      return field_;
    }

    [[nodiscard]] const std::vector<std::string>& path() const {
      return path_;
    }

  private:
    Field field_;
    std::vector<std::string> path_;
  };

  template <typename Entity> struct Predicate {
    using Field = typename EntityTraits<Entity>::Field;

    Field field;
    PredicateOperator op;
    nlohmann::json value;
    nlohmann::json lower;
    nlohmann::json upper;
    std::vector<nlohmann::json> values;
    std::vector<std::string> json_path;
  };

  template <typename Entity> struct SortSpec {
    using Field = typename EntityTraits<Entity>::Field;

    Field field;
    SortDirection direction{SortDirection::Ascending};
  };

  template <typename Entity> class Query {
  public:
    using Field = typename EntityTraits<Entity>::Field;

    [[nodiscard]] static Query all() {
      return Query();
    }

    [[nodiscard]] static Query where(Predicate<Entity> predicate) {
      Query query;
      query.predicates_.push_back(std::move(predicate));
      return query;
    }

    [[nodiscard]] Query and_where(Predicate<Entity> predicate) const {
      Query query = *this;
      query.predicates_.push_back(std::move(predicate));
      return query;
    }

    template <typename Value>
    [[nodiscard]] Query order_by(FieldRef<Entity, Value> field_ref,
                                 SortDirection direction = SortDirection::Ascending) const {
      Query query = *this;
      query.sorts_.push_back(SortSpec<Entity>{field_ref.field(), direction});
      return query;
    }

    [[nodiscard]] Query limit(std::size_t count) const {
      Query query = *this;
      query.limit_count_ = count;
      return query;
    }

    // Storage-layer escape hatch for deterministic test fixtures and admin
    // tooling. RPC handlers MUST use cursor-based pagination per TODO F8 —
    // OFFSET scans degrade badly on large tables and are unstable under
    // concurrent inserts/deletes.
    [[nodiscard]] Query offset(std::size_t count) const {
      Query query = *this;
      query.offset_count_ = count;
      return query;
    }

    [[nodiscard]] Query include_tombstoned() const {
      Query query = *this;
      query.include_tombstoned_ = true;
      return query;
    }

    [[nodiscard]] const std::vector<Predicate<Entity>>& predicates() const {
      return predicates_;
    }

    [[nodiscard]] const std::vector<SortSpec<Entity>>& sorts() const {
      return sorts_;
    }

    [[nodiscard]] std::optional<std::size_t> limit_count() const {
      return limit_count_;
    }

    [[nodiscard]] std::optional<std::size_t> offset_count() const {
      return offset_count_;
    }

    [[nodiscard]] bool includes_tombstoned() const {
      return include_tombstoned_;
    }

  private:
    std::vector<Predicate<Entity>> predicates_;
    std::vector<SortSpec<Entity>> sorts_;
    std::optional<std::size_t> limit_count_;
    std::optional<std::size_t> offset_count_;
    bool include_tombstoned_{false};
  };

  template <typename Entity, typename Value>
  [[nodiscard]] constexpr FieldRef<Entity, Value>
  field(typename EntityTraits<Entity>::Field field) {
    return FieldRef<Entity, Value>(field);
  }

  template <typename Entity>
  [[nodiscard]] JsonPathRef<Entity> json_path(typename EntityTraits<Entity>::Field field,
                                              std::vector<std::string> path) {
    return JsonPathRef<Entity>(field, std::move(path));
  }

  template <typename Entity, typename Value>
  [[nodiscard]] Predicate<Entity> operator==(FieldRef<Entity, Value> field_ref,
                                             const std::type_identity_t<Value>& value) {
    return Predicate<Entity>{
        .field = field_ref.field(),
        .op = PredicateOperator::Equal,
        .value = value,
    };
  }

  template <typename Entity>
  [[nodiscard]] Predicate<Entity> operator==(const JsonPathRef<Entity>& path_ref,
                                             std::string_view value) {
    return Predicate<Entity>{
        .field = path_ref.field(),
        .op = PredicateOperator::JsonPathEqual,
        .value = std::string(value),
        .json_path = path_ref.path(),
    };
  }

  template <typename Entity, typename Value>
  [[nodiscard]] Predicate<Entity> greater_or_equal(FieldRef<Entity, Value> field_ref,
                                                   const Value& value) {
    return Predicate<Entity>{
        .field = field_ref.field(),
        .op = PredicateOperator::GreaterThanOrEqual,
        .value = value,
    };
  }

  template <typename Entity, typename Value>
  [[nodiscard]] Predicate<Entity> less_or_equal(FieldRef<Entity, Value> field_ref,
                                                const Value& value) {
    return Predicate<Entity>{
        .field = field_ref.field(),
        .op = PredicateOperator::LessThanOrEqual,
        .value = value,
    };
  }

  template <typename Entity, typename Value>
  [[nodiscard]] Predicate<Entity> between(FieldRef<Entity, Value> field_ref, const Value& lower,
                                          const Value& upper) {
    return Predicate<Entity>{
        .field = field_ref.field(),
        .op = PredicateOperator::Between,
        .lower = lower,
        .upper = upper,
    };
  }

  template <typename Entity, typename Value>
  [[nodiscard]] Predicate<Entity> in(FieldRef<Entity, Value> field_ref,
                                     const std::vector<Value>& values) {
    std::vector<nlohmann::json> json_values;
    json_values.reserve(values.size());
    for (const auto& value : values) {
      json_values.push_back(value);
    }

    return Predicate<Entity>{
        .field = field_ref.field(),
        .op = PredicateOperator::In,
        .values = std::move(json_values),
    };
  }

  template <typename Entity, typename Value>
  [[nodiscard]] Predicate<Entity> FieldRef<Entity, Value>::between(const Value& lower,
                                                                   const Value& upper) const {
    return storage::between(*this, lower, upper);
  }

  template <typename Entity, typename Value>
  [[nodiscard]] Predicate<Entity>
  FieldRef<Entity, Value>::in(const std::vector<Value>& values) const {
    return storage::in(*this, values);
  }

  class IRepositoryBase {
  public:
    virtual ~IRepositoryBase() = default;
  };

  template <typename Entity> class IRepository : public IRepositoryBase {
  public:
    using Id = typename EntityTraits<Entity>::Id;

    ~IRepository() override = default;

    // NOLINTNEXTLINE(portability-template-virtual-member-function)
    [[nodiscard]] virtual std::optional<Entity> find_by_id(const Id& entity_id) = 0;
    // NOLINTNEXTLINE(portability-template-virtual-member-function)
    [[nodiscard]] virtual std::vector<Entity> query(const Query<Entity>& query_spec) = 0;
    // NOLINTNEXTLINE(portability-template-virtual-member-function)
    virtual void insert(const Entity& entity, const MutationContext& context) = 0;
    // NOLINTNEXTLINE(portability-template-virtual-member-function)
    virtual void update(const Entity& entity, const MutationContext& context) = 0;
    // NOLINTNEXTLINE(portability-template-virtual-member-function)
    virtual void soft_delete(const Id& entity_id, const MutationContext& context) = 0;
  };

  class ITransaction {
  public:
    virtual ~ITransaction() = default;

    virtual void commit() = 0;
    virtual void rollback() = 0;

    // Set a backend-specific session variable. No-op for SQLite; PostgresTransaction
    // overrides this to issue "SET LOCAL app.<key> = '<value>'" for RLS.
    virtual void set_session_var(std::string_view /*key*/, std::string_view /*value*/) {}

    // Append a PHI-read audit event (PRD §7.3). Unlike note_mutation this records a
    // *read*: it carries the disclosed PHI field KEY NAMES only — never the values
    // (PRD §17 redaction). The action is fixed to "phi.read" and the event joins the
    // same hash chain at commit. The default throws so a backend that has no audit
    // chain cannot silently drop a PHI-access record.
    virtual void note_phi_read(const std::string& /*entity_kind*/, const std::string& /*entity_id*/,
                               const MutationContext& /*context*/,
                               const std::vector<std::string>& /*field_keys*/) {
      throw UnsupportedOperation("note_phi_read is not supported by this backend");
    }

    // Record a mutation (or a non-domain event such as a `backup.*` record) for the
    // audit chain written at commit time. The snapshot is server-derived, never
    // caller-supplied PHI. The default throws so a backend with no audit chain
    // cannot silently drop the record; SqliteTransaction / PostgresTransaction
    // override it. Declared here so callers holding only an ITransaction& (e.g. the
    // backup engines) can append regardless of the concrete backend.
    // By-value parameters match the overriding signatures, which move them into
    // the pending-audit record; the throwing default ignores them.
    // NOLINTBEGIN(performance-unnecessary-value-param)
    virtual void note_mutation(std::string /*entity_kind*/, std::string /*entity_id*/,
                               const MutationContext& /*context*/,
                               std::string /*action*/ = "mutation",
                               AuditSnapshot /*snapshot*/ = {}) {
      throw UnsupportedOperation("note_mutation is not supported by this backend");
    }
    // NOLINTEND(performance-unnecessary-value-param)

    template <typename Entity> IRepository<Entity>& repo() {
      const auto iterator = repositories_.find(std::type_index(typeid(Entity)));
      if (iterator == repositories_.end()) {
        throw UnsupportedOperation("repository is not available for entity type");
      }
      return static_cast<IRepository<Entity>&>(*iterator->second);
    }

  protected:
    // type_index hash_code is stable per-process (C++ standard) — safe for single-process use.
    template <typename Entity>
    void register_repository(std::unique_ptr<IRepository<Entity>> repository) {
      repositories_.insert_or_assign(std::type_index(typeid(Entity)), std::move(repository));
    }

    std::unordered_map<std::type_index, std::unique_ptr<IRepositoryBase>> repositories_;
  };

  class IStorageBackend {
  public:
    virtual ~IStorageBackend() = default;

    virtual void migrate_to_latest() = 0;
    [[nodiscard]] virtual SchemaVersion current_version() const = 0;
    [[nodiscard]] virtual std::unique_ptr<ITransaction> begin(IsolationLevel isolation_level) = 0;
    [[nodiscard]] virtual Capabilities caps() const = 0;
  };

} // namespace fmgr::storage

#endif // FMGR_STORAGE_ISTORAGEBACKEND_H
