// SPDX-License-Identifier: AGPL-3.0-or-later

#include "storage/sqlite/ShareRequestRepositories.h"

#include "core/share_request.h"
#include "storage/ShareRequestTraits.h"

#include <sqlite3.h>

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fmgr::storage {
  namespace {

    // ---- SQLite helpers ----

    [[nodiscard]] std::string sqlite_error(sqlite3* handle, std::string_view action) {
      return std::string(action) + ": " + sqlite3_errmsg(handle);
    }

    [[noreturn]] void throw_sqlite_error(int code, sqlite3* handle, std::string_view action) {
      const auto extended_code = sqlite3_extended_errcode(handle);
      const auto effective_code = extended_code == SQLITE_OK ? code : extended_code;
      switch (effective_code) {
      case SQLITE_CONSTRAINT_UNIQUE:
      case SQLITE_CONSTRAINT_PRIMARYKEY:
        throw UniqueViolation(sqlite_error(handle, action));
      case SQLITE_CONSTRAINT_FOREIGNKEY:
        throw ForeignKeyViolation(sqlite_error(handle, action));
      case SQLITE_BUSY:
      case SQLITE_LOCKED:
        throw Unavailable(sqlite_error(handle, action));
      case SQLITE_CONSTRAINT:
      case SQLITE_CONSTRAINT_CHECK:
      case SQLITE_CONSTRAINT_NOTNULL:
        throw ConstraintViolation(sqlite_error(handle, action));
      default:
        throw BackendError(BackendErrorCode::ConstraintViolation, sqlite_error(handle, action));
      }
    }

    class Statement {
    public:
      Statement(sqlite3* handle, const std::string& sql) : handle_(handle) {
        const auto result = sqlite3_prepare_v2(handle_, sql.c_str(), -1, &statement_, nullptr);
        if (result != SQLITE_OK) {
          throw_sqlite_error(result, handle_, "prepare sqlite share_request statement");
        }
      }

      ~Statement() {
        sqlite3_finalize(statement_);
      }

      Statement(const Statement&) = delete;
      Statement& operator=(const Statement&) = delete;

      [[nodiscard]] sqlite3_stmt* get() const {
        return statement_;
      }

      [[nodiscard]] bool step_row() const {
        const auto result = sqlite3_step(statement_);
        if (result == SQLITE_ROW) {
          return true;
        }
        if (result == SQLITE_DONE) {
          return false;
        }
        throw_sqlite_error(result, handle_, "step sqlite share_request statement");
      }

      void step_done() const {
        const auto result = sqlite3_step(statement_);
        if (result != SQLITE_DONE) {
          throw_sqlite_error(result, handle_, "execute sqlite share_request statement");
        }
      }

    private:
      sqlite3* handle_;
      sqlite3_stmt* statement_{nullptr};
    };

    void bind_text(sqlite3_stmt* statement, int index, const std::string& value) {
      const auto result = sqlite3_bind_text(statement, index, value.c_str(),
                                            static_cast<int>(value.size()), SQLITE_TRANSIENT);
      if (result != SQLITE_OK) {
        throw ConstraintViolation("failed to bind sqlite text parameter");
      }
    }

    void bind_int64(sqlite3_stmt* statement, int index, std::int64_t value) {
      const auto result = sqlite3_bind_int64(statement, index, value);
      if (result != SQLITE_OK) {
        throw ConstraintViolation("failed to bind sqlite integer parameter");
      }
    }

    void bind_null(sqlite3_stmt* statement, int index) {
      const auto result = sqlite3_bind_null(statement, index);
      if (result != SQLITE_OK) {
        throw ConstraintViolation("failed to bind sqlite null parameter");
      }
    }

    void bind_optional_timestamp(sqlite3_stmt* statement, int index,
                                 const std::optional<core::Timestamp>& timestamp) {
      if (timestamp.has_value()) {
        bind_int64(statement, index, timestamp->unix_micros());
      } else {
        bind_null(statement, index);
      }
    }

    void bind_optional_text(sqlite3_stmt* statement, int index,
                            const std::optional<std::string>& value) {
      if (value.has_value()) {
        bind_text(statement, index, value.value());
      } else {
        bind_null(statement, index);
      }
    }

    [[nodiscard]] std::string column_text(sqlite3_stmt* statement, int column) {
      const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(statement, column));
      return text == nullptr ? std::string() : std::string(text);
    }

    [[nodiscard]] std::optional<std::string> column_optional_text(sqlite3_stmt* statement,
                                                                  int column) {
      if (sqlite3_column_type(statement, column) == SQLITE_NULL) {
        return std::nullopt;
      }
      return column_text(statement, column);
    }

    [[nodiscard]] std::optional<core::Timestamp> column_optional_timestamp(sqlite3_stmt* statement,
                                                                           int column) {
      if (sqlite3_column_type(statement, column) == SQLITE_NULL) {
        return std::nullopt;
      }
      return core::Timestamp::from_unix_micros(sqlite3_column_int64(statement, column));
    }

    [[nodiscard]] core::Timestamp now_timestamp() {
      const auto now =
          std::chrono::time_point_cast<std::chrono::microseconds>(std::chrono::system_clock::now());
      return core::Timestamp::from_unix_micros(now.time_since_epoch().count());
    }

    [[nodiscard]] std::string json_path(const std::vector<std::string>& segments) {
      std::string path = "$";
      for (const auto& segment : segments) {
        path += ".";
        path += segment;
      }
      return path;
    }

    void bind_json_parameter(sqlite3_stmt* statement, int index, const nlohmann::json& value) {
      if (value.is_null()) {
        bind_null(statement, index);
        return;
      }
      if (value.is_number_integer()) {
        bind_int64(statement, index, value.get<std::int64_t>());
        return;
      }
      if (value.is_string()) {
        bind_text(statement, index, value.get<std::string>());
        return;
      }
      bind_text(statement, index, value.dump());
    }

    void bind_parameters(sqlite3_stmt* statement, const std::vector<nlohmann::json>& parameters) {
      int index = 1;
      for (const auto& parameter : parameters) {
        bind_json_parameter(statement, index, parameter);
        ++index;
      }
    }

    // ---- Query tail builder ----

    template <typename Entity, typename ColumnName>
    void append_generic_predicates(std::string& sql, std::vector<nlohmann::json>& parameters,
                                   const std::vector<std::string>& default_predicates,
                                   const std::vector<Predicate<Entity>>& predicates,
                                   ColumnName column_name) {
      std::vector<std::string> clauses = default_predicates;
      for (const auto& predicate : predicates) {
        const auto column = column_name(predicate.field);
        switch (predicate.op) {
        case PredicateOperator::Equal:
          clauses.push_back(column + " = ?");
          parameters.push_back(predicate.value);
          break;
        case PredicateOperator::GreaterThanOrEqual:
          clauses.push_back(column + " >= ?");
          parameters.push_back(predicate.value);
          break;
        case PredicateOperator::LessThanOrEqual:
          clauses.push_back(column + " <= ?");
          parameters.push_back(predicate.value);
          break;
        case PredicateOperator::Between:
          clauses.push_back(column + " BETWEEN ? AND ?");
          parameters.push_back(predicate.lower);
          parameters.push_back(predicate.upper);
          break;
        case PredicateOperator::In: {
          std::string clause = column + " IN (";
          for (std::size_t i = 0; i < predicate.values.size(); ++i) {
            if (i != 0) {
              clause += ", ";
            }
            clause += "?";
            parameters.push_back(predicate.values.at(i));
          }
          clause += ")";
          clauses.push_back(std::move(clause));
          break;
        }
        case PredicateOperator::JsonPathEqual:
          clauses.push_back("json_extract(" + column + ", ?) = ?");
          parameters.emplace_back(json_path(predicate.json_path));
          parameters.push_back(predicate.value);
          break;
        }
      }
      if (!clauses.empty()) {
        sql += " WHERE ";
        for (std::size_t i = 0; i < clauses.size(); ++i) {
          if (i != 0) {
            sql += " AND ";
          }
          sql += clauses.at(i);
        }
      }
    }

    template <typename Entity, typename ColumnName>
    void append_query_tail(std::string& sql, std::vector<nlohmann::json>& parameters,
                           const Query<Entity>& query_spec, ColumnName column_name) {
      if (!query_spec.sorts().empty()) {
        sql += " ORDER BY ";
        for (std::size_t i = 0; i < query_spec.sorts().size(); ++i) {
          if (i != 0) {
            sql += ", ";
          }
          const auto sort = query_spec.sorts().at(i);
          sql += column_name(sort.field);
          sql += sort.direction == SortDirection::Ascending ? " ASC" : " DESC";
        }
      }
      if (query_spec.limit_count().has_value()) {
        sql += " LIMIT ?";
        parameters.emplace_back(static_cast<std::int64_t>(query_spec.limit_count().value()));
      }
      if (query_spec.offset_count().has_value()) {
        if (!query_spec.limit_count().has_value()) {
          sql += " LIMIT -1";
        }
        sql += " OFFSET ?";
        parameters.emplace_back(static_cast<std::int64_t>(query_spec.offset_count().value()));
      }
    }

    // ---- Base template for ShareRequest ----

    template <typename Entity> class SqliteShareRepositoryBase : public IRepository<Entity> {
    public:
      explicit SqliteShareRepositoryBase(SqliteTransaction& transaction)
          : transaction_(transaction) {
        transaction_.add_commit_hook([this](sqlite3* handle) { flush(handle); });
      }

    protected:
      struct PendingEntity {
        Entity entity;
        bool is_insert{false};
      };

      [[nodiscard]] SqliteTransaction& transaction() {
        return transaction_;
      }
      [[nodiscard]] const SqliteTransaction& transaction() const {
        return transaction_;
      }

      [[nodiscard]] const std::map<typename EntityTraits<Entity>::Id, PendingEntity>&
      pending() const {
        return pending_;
      }
      [[nodiscard]] std::map<typename EntityTraits<Entity>::Id, PendingEntity>& pending() {
        return pending_;
      }

      void stage_insert(const Entity& entity, const MutationContext& context) {
        const auto entity_id = id_of(entity);
        if (pending_.contains(entity_id) || load(entity_id).has_value()) {
          throw UniqueViolation(std::string(EntityTraits<Entity>::entity_name()) +
                                " id already exists");
        }
        validate(entity);
        pending_.insert_or_assign(entity_id, PendingEntity{.entity = entity, .is_insert = true});
        transaction_.note_mutation(std::string(EntityTraits<Entity>::entity_name()),
                                   entity_id.to_string(), context);
      }

      void stage_update(const Entity& entity, const MutationContext& context) {
        const auto entity_id = id_of(entity);
        if (!pending_.contains(entity_id) && !load(entity_id).has_value()) {
          throw NotFound(std::string(EntityTraits<Entity>::entity_name()) + " not found");
        }
        validate(entity);
        const auto is_insert = pending_.contains(entity_id) && pending_.at(entity_id).is_insert;
        pending_.insert_or_assign(entity_id,
                                  PendingEntity{.entity = entity, .is_insert = is_insert});
        transaction_.note_mutation(std::string(EntityTraits<Entity>::entity_name()),
                                   entity_id.to_string(), context);
      }

      [[nodiscard]] std::optional<Entity>
      find_staged(const typename EntityTraits<Entity>::Id& entity_id) const {
        const auto iter = pending_.find(entity_id);
        if (iter == pending_.end()) {
          return std::nullopt;
        }
        return iter->second.entity;
      }

    private:
      // NOLINTBEGIN(portability-template-virtual-member-function)
      virtual void flush(sqlite3* handle) = 0;
      [[nodiscard]] virtual std::optional<Entity>
      load(const typename EntityTraits<Entity>::Id& entity_id) const = 0;
      virtual void validate(const Entity& entity) const = 0;
      [[nodiscard]] virtual typename EntityTraits<Entity>::Id id_of(const Entity& entity) const = 0;
      // NOLINTEND(portability-template-virtual-member-function)

      SqliteTransaction& transaction_;
      std::map<typename EntityTraits<Entity>::Id, PendingEntity> pending_;
    };

    // ---- ShareRequest column mapping ----

    [[nodiscard]] std::string sr_column_name(core::ShareRequest::Field field) {
      switch (field) {
      case core::ShareRequest::Field::Id:
        return "id";
      case core::ShareRequest::Field::SourceLabId:
        return "source_lab_id";
      case core::ShareRequest::Field::TargetLabId:
        return "target_lab_id";
      case core::ShareRequest::Field::RequestedBy:
        return "requested_by";
      case core::ShareRequest::Field::ScopeJson:
        return "scope_json";
      case core::ShareRequest::Field::Status:
        return "status";
      case core::ShareRequest::Field::CreatedAt:
        return "created_at_micros";
      case core::ShareRequest::Field::DecidedAt:
        return "decided_at_micros";
      }
      throw ConstraintViolation("unknown share_request field");
    }

    constexpr std::string_view k_sr_columns =
        "id, source_lab_id, target_lab_id, requested_by, scope_json, "
        "status, created_at_micros, decided_at_micros";

    [[nodiscard]] core::ShareRequest read_sr(sqlite3_stmt* statement) {
      return core::ShareRequest{
          .id = core::ShareRequestId::parse(column_text(statement, 0)),
          .source_lab_id = core::LabId::parse(column_text(statement, 1)),
          .target_lab_id = core::LabId::parse(column_text(statement, 2)),
          .requested_by = core::UserId::parse(column_text(statement, 3)),
          .scope_json = column_text(statement, 4),
          .status = core::parse_share_request_status(column_text(statement, 5)),
          .created_at = core::Timestamp::from_unix_micros(sqlite3_column_int64(statement, 6)),
          .decided_at = column_optional_timestamp(statement, 7),
      };
    }

    // ---- ShareRequestRepository ----

    class ShareRequestRepository final : public SqliteShareRepositoryBase<core::ShareRequest> {
    public:
      using SqliteShareRepositoryBase::SqliteShareRepositoryBase;

      [[nodiscard]] std::optional<core::ShareRequest>
      find_by_id(const core::ShareRequestId& entity_id) override {
        if (auto staged = find_staged(entity_id); staged.has_value()) {
          return staged;
        }
        return load(entity_id);
      }

      [[nodiscard]] std::vector<core::ShareRequest>
      query(const Query<core::ShareRequest>& query_spec) override {
        std::string sql = "SELECT ";
        sql += k_sr_columns;
        sql += " FROM share_requests";
        std::vector<nlohmann::json> parameters;
        // Default: show only pending. includes_tombstoned() removes the filter
        // to expose approved/rejected/revoked records as well.
        const auto defaults = query_spec.includes_tombstoned()
                                  ? std::vector<std::string>{}
                                  : std::vector<std::string>{"status = 'pending'"};
        append_generic_predicates(sql, parameters, defaults, query_spec.predicates(),
                                  sr_column_name);
        append_query_tail(sql, parameters, query_spec, sr_column_name);

        Statement statement(transaction().handle(), sql);
        bind_parameters(statement.get(), parameters);

        std::vector<core::ShareRequest> results;
        while (statement.step_row()) {
          results.push_back(read_sr(statement.get()));
        }
        return results;
      }

      void insert(const core::ShareRequest& entity, const MutationContext& context) override {
        stage_insert(entity, context);
      }

      void update(const core::ShareRequest& entity, const MutationContext& context) override {
        stage_update(entity, context);
      }

      void soft_delete(const core::ShareRequestId& entity_id,
                       const MutationContext& context) override {
        auto entity = find_by_id(entity_id);
        if (!entity.has_value()) {
          throw NotFound("share_request not found");
        }
        entity->status = core::ShareRequestStatus::Revoked;
        entity->decided_at = now_timestamp();
        // Bypass validate() — revocation does not change structural fields.
        const auto is_insert = pending().contains(entity_id) && pending().at(entity_id).is_insert;
        pending().insert_or_assign(entity_id,
                                   PendingEntity{.entity = entity.value(), .is_insert = is_insert});
        transaction().note_mutation(std::string(EntityTraits<core::ShareRequest>::entity_name()),
                                    entity_id.to_string(), context);
      }

    private:
      void flush(sqlite3* handle) override {
        for (const auto& [unused_id, pending_entity] : pending()) {
          (void)unused_id;
          if (pending_entity.is_insert) {
            insert_pending(handle, pending_entity.entity);
          } else {
            update_pending(handle, pending_entity.entity);
          }
        }
      }

      [[nodiscard]] std::optional<core::ShareRequest>
      load(const core::ShareRequestId& entity_id) const override {
        std::string sql = "SELECT ";
        sql += k_sr_columns;
        sql += " FROM share_requests WHERE id = ?";
        Statement statement(transaction().handle(), sql);
        bind_text(statement.get(), 1, entity_id.to_string());
        if (!statement.step_row()) {
          return std::nullopt;
        }
        return read_sr(statement.get());
      }

      void validate(const core::ShareRequest& entity) const override {
        if (entity.scope_json.empty()) {
          throw ConstraintViolation("share_request scope_json is required");
        }
        if (entity.source_lab_id == entity.target_lab_id) {
          throw ConstraintViolation("source_lab_id and target_lab_id must differ");
        }
      }

      [[nodiscard]] core::ShareRequestId id_of(const core::ShareRequest& entity) const override {
        return entity.id;
      }

      static void bind_entity(sqlite3_stmt* statement, const core::ShareRequest& entity) {
        bind_text(statement, 1, entity.id.to_string());
        bind_text(statement, 2, entity.source_lab_id.to_string());
        bind_text(statement, 3, entity.target_lab_id.to_string());
        bind_text(statement, 4, entity.requested_by.to_string());
        bind_text(statement, 5, entity.scope_json);
        bind_text(statement, 6, std::string(core::to_string(entity.status)));
        bind_int64(statement, 7, entity.created_at.unix_micros());
        bind_optional_timestamp(statement, 8, entity.decided_at);
      }

      static void insert_pending(sqlite3* handle, const core::ShareRequest& entity) {
        Statement statement(handle, "INSERT INTO share_requests "
                                    "(id, source_lab_id, target_lab_id, requested_by, scope_json, "
                                    "status, created_at_micros, decided_at_micros) "
                                    "VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
        bind_entity(statement.get(), entity);
        statement.step_done();
      }

      static void update_pending(sqlite3* handle, const core::ShareRequest& entity) {
        Statement statement(handle,
                            "UPDATE share_requests SET "
                            "id = ?, source_lab_id = ?, target_lab_id = ?, requested_by = ?, "
                            "scope_json = ?, status = ?, created_at_micros = ?, "
                            "decided_at_micros = ? WHERE id = ?");
        bind_entity(statement.get(), entity);
        bind_text(statement.get(), 9, entity.id.to_string());
        statement.step_done();
      }
    };

    // ---- ShareRequestApproval column mapping ----

    [[nodiscard]] std::string approval_column_name(core::ShareRequestApproval::Field field) {
      switch (field) {
      case core::ShareRequestApproval::Field::ShareRequestId:
        return "share_request_id";
      case core::ShareRequestApproval::Field::ApproverRole:
        return "approver_role";
      case core::ShareRequestApproval::Field::ApproverUserId:
        return "approver_user_id";
      case core::ShareRequestApproval::Field::DecidedAt:
        return "decided_at_micros";
      case core::ShareRequestApproval::Field::Note:
        return "note";
      }
      throw ConstraintViolation("unknown share_request_approval field");
    }

    constexpr std::string_view k_approval_columns =
        "share_request_id, approver_role, approver_user_id, decided_at_micros, note";

    [[nodiscard]] core::ShareRequestApproval read_approval(sqlite3_stmt* statement) {
      return core::ShareRequestApproval{
          .share_request_id = core::ShareRequestId::parse(column_text(statement, 0)),
          .approver_role = core::parse_share_approval_role(column_text(statement, 1)),
          .approver_user_id = core::UserId::parse(column_text(statement, 2)),
          .decided_at = core::Timestamp::from_unix_micros(sqlite3_column_int64(statement, 3)),
          .note = column_optional_text(statement, 4),
      };
    }

    // ---- ShareRequestApprovalRepository ----
    // Append-only: update() and soft_delete() throw UnsupportedOperation.
    // Composite key (share_request_id, approver_role) — cannot use base template.

    class ShareRequestApprovalRepository final : public IRepository<core::ShareRequestApproval> {
    public:
      explicit ShareRequestApprovalRepository(SqliteTransaction& transaction)
          : transaction_(transaction) {
        transaction_.add_commit_hook([this](sqlite3* handle) { flush(handle); });
      }

      [[nodiscard]] std::optional<core::ShareRequestApproval>
      find_by_id(const core::ShareRequestApprovalId& entity_id) override {
        const auto iter = pending_.find(entity_id);
        if (iter != pending_.end()) {
          return iter->second;
        }
        return load(entity_id);
      }

      [[nodiscard]] std::vector<core::ShareRequestApproval>
      query(const Query<core::ShareRequestApproval>& query_spec) override {
        std::string sql = "SELECT ";
        sql += k_approval_columns;
        sql += " FROM share_request_approvals";
        std::vector<nlohmann::json> parameters;
        // No tombstone — includes_tombstoned() is irrelevant.
        append_generic_predicates(sql, parameters, {}, query_spec.predicates(),
                                  approval_column_name);
        append_query_tail(sql, parameters, query_spec, approval_column_name);

        Statement statement(transaction_.handle(), sql);
        bind_parameters(statement.get(), parameters);

        std::vector<core::ShareRequestApproval> results;
        while (statement.step_row()) {
          results.push_back(read_approval(statement.get()));
        }
        // Append in-memory inserts not yet flushed.
        for (const auto& [unused_id, approval] : pending_) {
          results.push_back(approval);
        }
        return results;
      }

      void insert(const core::ShareRequestApproval& entity,
                  const MutationContext& context) override {
        const auto approval_id = entity.id();
        if (pending_.contains(approval_id) || load(approval_id).has_value()) {
          throw UniqueViolation("share_request_approval already exists for this role");
        }
        // share_request must exist in committed DB (cross-entity refs need committed state).
        {
          Statement stmt(transaction_.handle(),
                         "SELECT 1 FROM share_requests WHERE id = ? LIMIT 1");
          bind_text(stmt.get(), 1, entity.share_request_id.to_string());
          if (!stmt.step_row()) {
            throw ConstraintViolation(
                "share_request_id does not reference a committed share_request");
          }
        }
        pending_.insert_or_assign(approval_id, entity);
        const auto composite_key = entity.share_request_id.to_string() + ":" +
                                   std::string(core::to_string(entity.approver_role));
        transaction_.note_mutation("share_request_approval", composite_key, context);
      }

      void update(const core::ShareRequestApproval& /*entity*/,
                  const MutationContext& /*context*/) override {
        throw UnsupportedOperation("ShareRequestApproval rows are immutable (append-only audit)");
      }

      void soft_delete(const core::ShareRequestApprovalId& /*entity_id*/,
                       const MutationContext& /*context*/) override {
        throw UnsupportedOperation("ShareRequestApproval rows are immutable (append-only audit)");
      }

    private:
      [[nodiscard]] std::optional<core::ShareRequestApproval>
      load(const core::ShareRequestApprovalId& entity_id) const {
        std::string sql = "SELECT ";
        sql += k_approval_columns;
        sql += " FROM share_request_approvals WHERE share_request_id = ? AND approver_role = ?";
        Statement statement(transaction_.handle(), sql);
        bind_text(statement.get(), 1, entity_id.share_request_id.to_string());
        bind_text(statement.get(), 2, std::string(core::to_string(entity_id.approver_role)));
        if (!statement.step_row()) {
          return std::nullopt;
        }
        return read_approval(statement.get());
      }

      void flush(sqlite3* handle) {
        for (const auto& [unused_id, approval] : pending_) {
          (void)unused_id;
          Statement statement(handle, "INSERT INTO share_request_approvals "
                                      "(share_request_id, approver_role, approver_user_id, "
                                      "decided_at_micros, note) "
                                      "VALUES (?, ?, ?, ?, ?)");
          bind_text(statement.get(), 1, approval.share_request_id.to_string());
          bind_text(statement.get(), 2, std::string(core::to_string(approval.approver_role)));
          bind_text(statement.get(), 3, approval.approver_user_id.to_string());
          bind_int64(statement.get(), 4, approval.decided_at.unix_micros());
          bind_optional_text(statement.get(), 5, approval.note);
          statement.step_done();
        }
      }

      SqliteTransaction& transaction_;
      std::map<core::ShareRequestApprovalId, core::ShareRequestApproval> pending_;
    };

  } // namespace

  void register_share_request_repositories(SqliteBackend& backend) {
    backend.register_repository_factory<core::ShareRequest>([](SqliteTransaction& transaction) {
      return std::make_unique<ShareRequestRepository>(transaction);
    });
    backend.register_repository_factory<core::ShareRequestApproval>(
        [](SqliteTransaction& transaction) {
          return std::make_unique<ShareRequestApprovalRepository>(transaction);
        });
  }

} // namespace fmgr::storage
