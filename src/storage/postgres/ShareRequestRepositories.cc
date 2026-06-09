// SPDX-License-Identifier: AGPL-3.0-or-later

#include "storage/postgres/ShareRequestRepositories.h"

#include "core/share_request.h"
#include "storage/ShareRequestTraits.h"
#include "storage/detail/QuerySqlBuilder.h"
#include "storage/detail/ShareRequestColumns.h"
#include "storage/postgres/PostgresRepoSupport.h"

#include <pqxx/pqxx>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fmgr::storage {
  namespace {

    using detail::micros_or_null;
    using detail::pg_bind_params;
    using detail::pg_optional_string;
    using detail::pg_optional_timestamp;
    using detail::throw_pqxx_error;
    using detail::validate_share_request;

    [[nodiscard]] core::Timestamp now_timestamp() {
      const auto now =
          std::chrono::time_point_cast<std::chrono::microseconds>(std::chrono::system_clock::now());
      return core::Timestamp::from_unix_micros(now.time_since_epoch().count());
    }

    // ---- ShareRequest ----

    constexpr const char* share_request_columns =
        "id, source_lab_id, target_lab_id, requested_by, scope_json, status, created_at_micros, "
        "decided_at_micros";

    [[nodiscard]] core::ShareRequest read_share_request(pqxx::row_ref row) {
      return core::ShareRequest{
          .id = core::ShareRequestId::parse(row.at("id").as<std::string>()),
          .source_lab_id = core::LabId::parse(row.at("source_lab_id").as<std::string>()),
          .target_lab_id = core::LabId::parse(row.at("target_lab_id").as<std::string>()),
          .requested_by = core::UserId::parse(row.at("requested_by").as<std::string>()),
          .scope_json = row.at("scope_json").as<std::string>(),
          .status = core::parse_share_request_status(row.at("status").as<std::string>()),
          .created_at =
              core::Timestamp::from_unix_micros(row.at("created_at_micros").as<std::int64_t>()),
          .decided_at = pg_optional_timestamp(row, "decided_at_micros"),
      };
    }

    [[nodiscard]] pqxx::params bind_share_request(const core::ShareRequest& entity) {
      pqxx::params params;
      params.append(entity.id.to_string());
      params.append(entity.source_lab_id.to_string());
      params.append(entity.target_lab_id.to_string());
      params.append(entity.requested_by.to_string());
      params.append(entity.scope_json);
      params.append(std::string(core::to_string(entity.status)));
      params.append(entity.created_at.unix_micros());
      params.append(micros_or_null(entity.decided_at));
      return params;
    }

    class ShareRequestRepository final : public IRepository<core::ShareRequest> {
    public:
      explicit ShareRequestRepository(PostgresTransaction& txn) : txn_(txn) {}

      [[nodiscard]] std::optional<core::ShareRequest>
      find_by_id(const core::ShareRequestId& entity_id) override {
        try {
          const auto result = txn_.work().exec(std::string("SELECT ") + share_request_columns +
                                                   " FROM share_requests WHERE id = $1",
                                               pqxx::params{entity_id.to_string()});
          if (result.empty()) {
            return std::nullopt;
          }
          return read_share_request(result[0]);
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
      }

      [[nodiscard]] std::vector<core::ShareRequest>
      query(const Query<core::ShareRequest>& query_spec) override {
        std::string sql = std::string("SELECT ") + share_request_columns + " FROM share_requests";
        std::vector<nlohmann::json> parameters;
        // ShareRequest "tombstone" default is the pending state.
        const auto defaults = query_spec.includes_tombstoned()
                                  ? std::vector<std::string>{}
                                  : std::vector<std::string>{"status = 'pending'"};
        detail::PostgresDialect dialect;
        detail::append_where(sql, parameters, defaults, query_spec.predicates(),
                             detail::share_request_column_name, dialect);
        detail::append_order_limit(sql, parameters, query_spec, detail::share_request_column_name,
                                   dialect);
        try {
          const auto result = txn_.work().exec(sql, pg_bind_params(parameters));
          std::vector<core::ShareRequest> results;
          for (pqxx::row_ref row : result) {
            results.push_back(read_share_request(row));
          }
          return results;
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
      }

      void insert(const core::ShareRequest& entity, const MutationContext& context) override {
        validate_share_request(entity);
        try {
          txn_.work().exec(
              "INSERT INTO share_requests (id, source_lab_id, target_lab_id, "
              "requested_by, scope_json, status, created_at_micros, decided_at_micros) "
              "VALUES ($1, $2, $3, $4, $5, $6, $7, $8)",
              bind_share_request(entity));
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
        txn_.note_mutation(std::string(EntityTraits<core::ShareRequest>::entity_name()),
                           entity.id.to_string(), context, "insert", detail::audit_after(entity));
      }

      void update(const core::ShareRequest& entity, const MutationContext& context) override {
        validate_share_request(entity);
        const auto before = find_by_id(entity.id);
        write_update(entity, context, before);
      }

      void soft_delete(const core::ShareRequestId& entity_id,
                       const MutationContext& context) override {
        auto entity = find_by_id(entity_id);
        if (!entity.has_value()) {
          throw NotFound("share_request not found");
        }
        const auto before = entity;
        entity->status = core::ShareRequestStatus::Revoked;
        entity->decided_at = now_timestamp();
        // Revocation does not change structural fields; skip scope/lab validation.
        write_update(entity.value(), context, before);
      }

    private:
      void write_update(const core::ShareRequest& entity, const MutationContext& context,
                        const std::optional<core::ShareRequest>& before) {
        try {
          const auto result = txn_.work().exec(
              "UPDATE share_requests SET source_lab_id = $2, target_lab_id = $3, "
              "requested_by = $4, scope_json = $5, status = $6, created_at_micros = $7, "
              "decided_at_micros = $8 WHERE id = $1",
              bind_share_request(entity));
          if (result.affected_rows() == 0) {
            throw NotFound("share_request not found");
          }
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
        txn_.note_mutation(std::string(EntityTraits<core::ShareRequest>::entity_name()),
                           entity.id.to_string(), context, "soft_delete",
                           detail::audit_change(before, entity));
      }

      PostgresTransaction& txn_;
    };

    // ---- ShareRequestApproval (append-only, composite key) ----

    constexpr const char* approval_columns =
        "share_request_id, approver_role, approver_user_id, decided_at_micros, note";

    [[nodiscard]] core::ShareRequestApproval read_approval(pqxx::row_ref row) {
      return core::ShareRequestApproval{
          .share_request_id =
              core::ShareRequestId::parse(row.at("share_request_id").as<std::string>()),
          .approver_role =
              core::parse_share_approval_role(row.at("approver_role").as<std::string>()),
          .approver_user_id = core::UserId::parse(row.at("approver_user_id").as<std::string>()),
          .decided_at =
              core::Timestamp::from_unix_micros(row.at("decided_at_micros").as<std::int64_t>()),
          .note = pg_optional_string(row, "note"),
      };
    }

    class ShareRequestApprovalRepository final : public IRepository<core::ShareRequestApproval> {
    public:
      explicit ShareRequestApprovalRepository(PostgresTransaction& txn) : txn_(txn) {}

      [[nodiscard]] std::optional<core::ShareRequestApproval>
      find_by_id(const core::ShareRequestApprovalId& entity_id) override {
        try {
          const auto result =
              txn_.work().exec(std::string("SELECT ") + approval_columns +
                                   " FROM share_request_approvals WHERE share_request_id = $1 AND "
                                   "approver_role = $2",
                               pqxx::params{entity_id.share_request_id.to_string(),
                                            std::string(core::to_string(entity_id.approver_role))});
          if (result.empty()) {
            return std::nullopt;
          }
          return read_approval(result[0]);
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
      }

      [[nodiscard]] std::vector<core::ShareRequestApproval>
      query(const Query<core::ShareRequestApproval>& query_spec) override {
        std::string sql =
            std::string("SELECT ") + approval_columns + " FROM share_request_approvals";
        std::vector<nlohmann::json> parameters;
        detail::PostgresDialect dialect;
        detail::append_where(sql, parameters, {}, query_spec.predicates(),
                             detail::share_approval_column_name, dialect);
        detail::append_order_limit(sql, parameters, query_spec, detail::share_approval_column_name,
                                   dialect);
        try {
          const auto result = txn_.work().exec(sql, pg_bind_params(parameters));
          std::vector<core::ShareRequestApproval> results;
          for (pqxx::row_ref row : result) {
            results.push_back(read_approval(row));
          }
          return results;
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
      }

      void insert(const core::ShareRequestApproval& entity,
                  const MutationContext& context) override {
        // The parent share_request must already exist (mirrors SQLite, which
        // checks committed state before staging the append).
        if (txn_.work()
                .exec("SELECT 1 FROM share_requests WHERE id = $1 LIMIT 1",
                      pqxx::params{entity.share_request_id.to_string()})
                .empty()) {
          throw ConstraintViolation(
              "share_request_id does not reference a committed share_request");
        }
        try {
          pqxx::params params;
          params.append(entity.share_request_id.to_string());
          params.append(std::string(core::to_string(entity.approver_role)));
          params.append(entity.approver_user_id.to_string());
          params.append(entity.decided_at.unix_micros());
          params.append(entity.note);
          txn_.work().exec("INSERT INTO share_request_approvals (share_request_id, approver_role, "
                           "approver_user_id, decided_at_micros, note) VALUES ($1, $2, $3, $4, $5)",
                           params);
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
        const auto composite_key = entity.share_request_id.to_string() + ":" +
                                   std::string(core::to_string(entity.approver_role));
        txn_.note_mutation("share_request_approval", composite_key, context, "insert",
                           detail::audit_after(entity));
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
      PostgresTransaction& txn_;
    };

  } // namespace

  void register_share_request_repositories(PostgresBackend& backend) {
    backend.register_repository_factory<core::ShareRequest>(
        [](PostgresTransaction& txn) { return std::make_unique<ShareRequestRepository>(txn); });
    backend.register_repository_factory<core::ShareRequestApproval>([](PostgresTransaction& txn) {
      return std::make_unique<ShareRequestApprovalRepository>(txn);
    });
  }

} // namespace fmgr::storage
