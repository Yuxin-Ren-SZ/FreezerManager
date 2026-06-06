// SPDX-License-Identifier: AGPL-3.0-or-later

#include "storage/postgres/AuditRepositories.h"

#include "core/audit_event.h"
#include "storage/AuditTraits.h"
#include "storage/detail/AuditColumns.h"
#include "storage/postgres/PostgresRepoSupport.h"

#include <pqxx/pqxx>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fmgr::storage {
  namespace {

    using detail::pg_optional_id;
    using detail::pg_optional_string;
    using detail::throw_pqxx_error;

    constexpr const char* audit_columns =
        "id, at_micros, actor_user_id, actor_session_id, lab_id, action, entity_kind, entity_id, "
        "before_json, after_json, request_id, prev_hash, this_hash";

    [[nodiscard]] core::AuditEvent read_event(pqxx::row_ref row) {
      return core::AuditEvent{
          .id = core::AuditEventId::parse(row.at("id").as<std::string>()),
          .at = core::Timestamp::from_unix_micros(row.at("at_micros").as<std::int64_t>()),
          .actor_user_id = core::UserId::parse(row.at("actor_user_id").as<std::string>()),
          .actor_session_id = row.at("actor_session_id").as<std::string>(),
          .lab_id = pg_optional_id<core::LabId>(row, "lab_id"),
          .action = row.at("action").as<std::string>(),
          .entity_kind = row.at("entity_kind").as<std::string>(),
          .entity_id = pg_optional_string(row, "entity_id"),
          .before_json = row.at("before_json").as<std::string>(),
          .after_json = row.at("after_json").as<std::string>(),
          .request_id = row.at("request_id").as<std::string>(),
          .prev_hash = row.at("prev_hash").as<std::string>(),
          .this_hash = row.at("this_hash").as<std::string>(),
      };
    }

    class AuditEventRepository final : public IRepository<core::AuditEvent> {
    public:
      explicit AuditEventRepository(PostgresTransaction& txn) : txn_(txn) {}

      [[nodiscard]] std::optional<core::AuditEvent>
      find_by_id(const core::AuditEventId& entity_id) override {
        try {
          const auto result = txn_.work().exec(std::string("SELECT ") + audit_columns +
                                                   " FROM audit_events WHERE id = $1",
                                               pqxx::params{entity_id.to_string()});
          if (result.empty()) {
            return std::nullopt;
          }
          return read_event(result[0]);
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
      }

      [[nodiscard]] std::vector<core::AuditEvent>
      query(const Query<core::AuditEvent>& query_spec) override {
        // Audit events are never soft-deleted; all rows are always visible.
        // Equality predicates only, matching the SQLite audit repository.
        std::string sql = std::string("SELECT ") + audit_columns + " FROM audit_events";
        pqxx::params params;
        int ordinal = 0;
        for (std::size_t index = 0; index < query_spec.predicates().size(); ++index) {
          const auto& predicate = query_spec.predicates().at(index);
          sql += index == 0 ? " WHERE " : " AND ";
          sql +=
              detail::audit_event_column_name(predicate.field) + " = $" + std::to_string(++ordinal);
          if (predicate.value.is_string()) {
            params.append(predicate.value.get<std::string>());
          } else {
            params.append(predicate.value.get<std::int64_t>());
          }
        }
        if (!query_spec.sorts().empty()) {
          sql += " ORDER BY ";
          for (std::size_t index = 0; index < query_spec.sorts().size(); ++index) {
            if (index != 0) {
              sql += ", ";
            }
            const auto& sort_spec = query_spec.sorts().at(index);
            sql += detail::audit_event_column_name(sort_spec.field);
            sql += sort_spec.direction == SortDirection::Ascending ? " ASC" : " DESC";
          }
        }
        if (query_spec.limit_count().has_value()) {
          sql += " LIMIT " + std::to_string(query_spec.limit_count().value());
        }
        if (query_spec.offset_count().has_value()) {
          sql += " OFFSET " + std::to_string(query_spec.offset_count().value());
        }

        try {
          const auto result = txn_.work().exec(sql, params);
          std::vector<core::AuditEvent> results;
          for (pqxx::row_ref row : result) {
            results.push_back(read_event(row));
          }
          return results;
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
      }

      void insert(const core::AuditEvent& /*entity*/, const MutationContext& /*context*/) override {
        throw UnsupportedOperation("AuditEvent rows are written by PostgresTransaction::commit(); "
                                   "do not insert them manually");
      }

      void update(const core::AuditEvent& /*entity*/, const MutationContext& /*context*/) override {
        throw UnsupportedOperation("audit_events is append-only");
      }

      void soft_delete(const core::AuditEventId& /*entity_id*/,
                       const MutationContext& /*context*/) override {
        throw UnsupportedOperation("audit_events is append-only");
      }

    private:
      PostgresTransaction& txn_;
    };

  } // namespace

  void register_audit_repositories(PostgresBackend& backend) {
    backend.register_repository_factory<core::AuditEvent>(
        [](PostgresTransaction& txn) { return std::make_unique<AuditEventRepository>(txn); });
  }

} // namespace fmgr::storage
