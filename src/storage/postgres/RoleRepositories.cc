// SPDX-License-Identifier: AGPL-3.0-or-later

#include "storage/postgres/RoleRepositories.h"

#include "core/role.h"
#include "storage/RoleTraits.h"
#include "storage/detail/RoleColumns.h"
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

    using detail::id_or_null;
    using detail::micros_or_null;
    using detail::pg_optional_id;
    using detail::pg_optional_timestamp;
    using detail::throw_pqxx_error;
    using detail::validate_role;

    [[nodiscard]] core::Timestamp now_timestamp() {
      const auto now =
          std::chrono::time_point_cast<std::chrono::microseconds>(std::chrono::system_clock::now());
      return core::Timestamp::from_unix_micros(now.time_since_epoch().count());
    }

    constexpr const char* role_columns =
        "id, lab_id, kind, name, description, is_builtin, created_at_micros, archived_at_micros";

    [[nodiscard]] core::Role read_role(pqxx::row_ref row) {
      return core::Role{
          .id = core::RoleId::parse(row.at("id").as<std::string>()),
          .lab_id = pg_optional_id<core::LabId>(row, "lab_id"),
          .kind = core::parse_role_kind(row.at("kind").as<std::string>()),
          .name = row.at("name").as<std::string>(),
          .description = row.at("description").as<std::string>(),
          .is_builtin = row.at("is_builtin").as<bool>(),
          .created_at =
              core::Timestamp::from_unix_micros(row.at("created_at_micros").as<std::int64_t>()),
          .archived_at = pg_optional_timestamp(row, "archived_at_micros"),
      };
    }

    [[nodiscard]] core::RolePermission read_role_permission(pqxx::row_ref row) {
      return core::RolePermission{
          .role_id = core::RoleId::parse(row.at("role_id").as<std::string>()),
          .permission = core::parse_permission(row.at("permission_key").as<std::string>()),
      };
    }

    class RoleRepository final : public IRepository<core::Role> {
    public:
      explicit RoleRepository(PostgresTransaction& txn) : txn_(txn) {}

      [[nodiscard]] std::optional<core::Role> find_by_id(const core::RoleId& entity_id) override {
        try {
          const auto result =
              txn_.work().exec(std::string("SELECT ") + role_columns + " FROM roles WHERE id = $1",
                               pqxx::params{entity_id.to_string()});
          if (result.empty()) {
            return std::nullopt;
          }
          return read_role(result[0]);
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
      }

      [[nodiscard]] std::vector<core::Role> query(const Query<core::Role>& query_spec) override {
        std::string sql = std::string("SELECT ") + role_columns + " FROM roles";
        if (!query_spec.includes_tombstoned()) {
          sql += " WHERE archived_at_micros IS NULL";
        }
        sql += " ORDER BY created_at_micros ASC, id ASC";
        if (const auto limit = query_spec.limit_count(); limit.has_value()) {
          sql += " LIMIT " + std::to_string(limit.value());
        }

        try {
          const auto result = txn_.work().exec(sql);
          std::vector<core::Role> results;
          for (pqxx::row_ref row : result) {
            results.push_back(read_role(row));
          }
          return results;
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
      }

      void insert(const core::Role& entity, const MutationContext& context) override {
        validate_role(entity);
        try {
          txn_.work().exec("INSERT INTO roles (id, lab_id, kind, name, description, is_builtin, "
                           "created_at_micros, archived_at_micros) "
                           "VALUES ($1, $2, $3, $4, $5, $6, $7, $8)",
                           bind_role(entity));
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
        txn_.note_mutation(std::string(EntityTraits<core::Role>::entity_name()),
                           entity.id.to_string(), context);
      }

      void update(const core::Role& entity, const MutationContext& context) override {
        validate_role(entity);
        try {
          const auto result =
              txn_.work().exec("UPDATE roles SET lab_id = $2, kind = $3, name = $4, "
                               "description = $5, is_builtin = $6, created_at_micros = $7, "
                               "archived_at_micros = $8 WHERE id = $1",
                               bind_role(entity));
          if (result.affected_rows() == 0) {
            throw NotFound("role not found");
          }
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
        txn_.note_mutation(std::string(EntityTraits<core::Role>::entity_name()),
                           entity.id.to_string(), context);
      }

      void soft_delete(const core::RoleId& entity_id, const MutationContext& context) override {
        auto entity = find_by_id(entity_id);
        if (!entity.has_value()) {
          throw NotFound("role not found");
        }
        if (entity->is_builtin) {
          throw ConstraintViolation("built-in roles cannot be archived");
        }
        entity->archived_at = now_timestamp();
        update(entity.value(), context);
      }

    private:
      [[nodiscard]] static pqxx::params bind_role(const core::Role& entity) {
        pqxx::params params;
        params.append(entity.id.to_string());
        params.append(id_or_null(entity.lab_id));
        params.append(std::string(core::to_string(entity.kind)));
        params.append(entity.name);
        params.append(entity.description);
        params.append(entity.is_builtin);
        params.append(entity.created_at.unix_micros());
        params.append(micros_or_null(entity.archived_at));
        return params;
      }

      PostgresTransaction& txn_;
    };

    // Bump the authorization epoch of every user holding the given role in this
    // transaction, so a permission grant/revoke on the role invalidates their
    // cached SessionContext on the next request (mirrors SQLite
    // detail::bump_authz_version_for_role).
    void bump_authz_version_for_role(PostgresTransaction& txn, const core::RoleId& role_id) {
      txn.work().exec("UPDATE users SET authz_version = authz_version + 1 WHERE id IN "
                      "(SELECT user_id FROM lab_memberships "
                      " WHERE role_id = $1 AND revoked_at_micros IS NULL)",
                      pqxx::params{role_id.to_string()});
    }

    class RolePermissionRepository final : public IRepository<core::RolePermission> {
    public:
      explicit RolePermissionRepository(PostgresTransaction& txn) : txn_(txn) {}

      [[nodiscard]] std::optional<core::RolePermission>
      find_by_id(const core::RolePermissionId& entity_id) override {
        try {
          const auto result =
              txn_.work().exec("SELECT role_id, permission_key FROM role_permissions "
                               "WHERE role_id = $1 AND permission_key = $2",
                               pqxx::params{entity_id.role_id.to_string(),
                                            std::string(core::to_key(entity_id.permission))});
          if (result.empty()) {
            return std::nullopt;
          }
          return read_role_permission(result[0]);
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
      }

      [[nodiscard]] std::vector<core::RolePermission>
      query(const Query<core::RolePermission>& query_spec) override {
        std::string sql = "SELECT role_id, permission_key FROM role_permissions";
        pqxx::params params;
        int ordinal = 0;
        for (std::size_t index = 0; index < query_spec.predicates().size(); ++index) {
          const auto& predicate = query_spec.predicates().at(index);
          if (predicate.op != PredicateOperator::Equal) {
            throw UnsupportedOperation("role_permission query supports only equality predicates");
          }
          sql += index == 0 ? " WHERE " : " AND ";
          switch (predicate.field) {
          case core::RolePermission::Field::RoleId:
            sql += "role_id = $" + std::to_string(++ordinal);
            break;
          case core::RolePermission::Field::PermissionKey:
            sql += "permission_key = $" + std::to_string(++ordinal);
            break;
          }
          params.append(predicate.value.is_string() ? predicate.value.get<std::string>()
                                                    : predicate.value.dump());
        }
        sql += " ORDER BY role_id, permission_key";

        try {
          const auto result = txn_.work().exec(sql, params);
          std::vector<core::RolePermission> results;
          for (pqxx::row_ref row : result) {
            results.push_back(read_role_permission(row));
          }
          return results;
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
      }

      void insert(const core::RolePermission& entity, const MutationContext& context) override {
        try {
          txn_.work().exec("INSERT INTO role_permissions (role_id, permission_key) VALUES ($1, $2)",
                           pqxx::params{entity.role_id.to_string(),
                                        std::string(core::to_key(entity.permission))});
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
        bump_authz_version_for_role(txn_, entity.role_id);
        txn_.note_mutation(std::string(EntityTraits<core::RolePermission>::entity_name()),
                           entity.id().to_string(), context);
      }

      void update(const core::RolePermission& /*entity*/,
                  const MutationContext& /*context*/) override {
        // RolePermission rows have only their composite key as state; updating
        // them has no semantic meaning. Callers should delete + re-insert.
        throw UnsupportedOperation("role_permission rows are immutable; insert + delete instead");
      }

      void soft_delete(const core::RolePermissionId& entity_id,
                       const MutationContext& context) override {
        try {
          const auto result = txn_.work().exec(
              "DELETE FROM role_permissions WHERE role_id = $1 AND permission_key = $2",
              pqxx::params{entity_id.role_id.to_string(),
                           std::string(core::to_key(entity_id.permission))});
          if (result.affected_rows() == 0) {
            throw NotFound("role permission grant not found");
          }
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
        bump_authz_version_for_role(txn_, entity_id.role_id);
        txn_.note_mutation(std::string(EntityTraits<core::RolePermission>::entity_name()),
                           entity_id.to_string(), context);
      }

    private:
      PostgresTransaction& txn_;
    };

  } // namespace

  void register_role_repositories(PostgresBackend& backend) {
    backend.register_repository_factory<core::Role>(
        [](PostgresTransaction& txn) { return std::make_unique<RoleRepository>(txn); });
    backend.register_repository_factory<core::RolePermission>(
        [](PostgresTransaction& txn) { return std::make_unique<RolePermissionRepository>(txn); });
  }

} // namespace fmgr::storage
