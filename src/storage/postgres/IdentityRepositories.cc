// SPDX-License-Identifier: AGPL-3.0-or-later

#include "storage/postgres/IdentityRepositories.h"

#include "core/identity.h"
#include "storage/IdentityTraits.h"
#include "storage/detail/IdentityColumns.h"
#include "storage/detail/QuerySqlBuilder.h"
#include "storage/postgres/PostgresRepoSupport.h"

#include <pqxx/pqxx>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fmgr::storage {
  namespace {

    using detail::id_or_null;
    using detail::micros_or_null;
    using detail::pg_bind_params;
    using detail::pg_optional_id;
    using detail::pg_optional_string;
    using detail::pg_optional_timestamp;
    using detail::throw_pqxx_error;

    [[nodiscard]] core::Timestamp now_timestamp() {
      const auto now =
          std::chrono::time_point_cast<std::chrono::microseconds>(std::chrono::system_clock::now());
      return core::Timestamp::from_unix_micros(now.time_since_epoch().count());
    }

    // ---- Row readers ----

    [[nodiscard]] core::Lab read_lab(pqxx::row_ref row) {
      return core::Lab{
          .id = core::LabId::parse(row.at("id").as<std::string>()),
          .name = row.at("name").as<std::string>(),
          .contact = row.at("contact").as<std::string>(),
          .created_at =
              core::Timestamp::from_unix_micros(row.at("created_at_micros").as<std::int64_t>()),
          .settings_json = nlohmann::json::parse(row.at("settings_json").as<std::string>()),
          .is_phi_enabled = row.at("is_phi_enabled").as<bool>(),
          .archived_at = pg_optional_timestamp(row, "archived_at_micros"),
      };
    }

    [[nodiscard]] core::User read_user(pqxx::row_ref row) {
      return core::User{
          .id = core::UserId::parse(row.at("id").as<std::string>()),
          .primary_email = row.at("primary_email").as<std::string>(),
          .display_name = row.at("display_name").as<std::string>(),
          .status = core::parse_user_status(row.at("status").as<std::string>()),
          .created_at =
              core::Timestamp::from_unix_micros(row.at("created_at_micros").as<std::int64_t>()),
          .auth_bindings = nlohmann::json::parse(row.at("auth_bindings_json").as<std::string>()),
          .totp_secret_enc = pg_optional_string(row, "totp_secret_enc"),
          .default_lab_id = pg_optional_id<core::LabId>(row, "default_lab_id"),
          .authz_version = row.at("authz_version").as<std::int64_t>(),
      };
    }

    [[nodiscard]] core::LabMembership read_membership(pqxx::row_ref row) {
      return core::LabMembership{
          .user_id = core::UserId::parse(row.at("user_id").as<std::string>()),
          .lab_id = core::LabId::parse(row.at("lab_id").as<std::string>()),
          .role_id = pg_optional_id<core::RoleId>(row, "role_id"),
          .scope_filters_json =
              nlohmann::json::parse(row.at("scope_filters_json").as<std::string>()),
          .invited_by = pg_optional_id<core::UserId>(row, "invited_by"),
          .joined_at =
              core::Timestamp::from_unix_micros(row.at("joined_at_micros").as<std::int64_t>()),
          .revoked_at = pg_optional_timestamp(row, "revoked_at_micros"),
      };
    }

    // ---- Repositories ----

    constexpr const char* lab_columns =
        "id, name, contact, created_at_micros, settings_json, is_phi_enabled, archived_at_micros";

    class LabRepository final : public IRepository<core::Lab> {
    public:
      explicit LabRepository(PostgresTransaction& txn) : txn_(txn) {}

      [[nodiscard]] std::optional<core::Lab> find_by_id(const core::LabId& entity_id) override {
        try {
          const auto result =
              txn_.work().exec(std::string("SELECT ") + lab_columns + " FROM labs WHERE id = $1",
                               pqxx::params{entity_id.to_string()});
          if (result.empty()) {
            return std::nullopt;
          }
          return read_lab(result[0]);
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
      }

      [[nodiscard]] std::vector<core::Lab> query(const Query<core::Lab>& query_spec) override {
        std::string sql = std::string("SELECT ") + lab_columns + " FROM labs";
        std::vector<nlohmann::json> parameters;
        const auto defaults = query_spec.includes_tombstoned()
                                  ? std::vector<std::string>{}
                                  : std::vector<std::string>{"archived_at_micros IS NULL"};
        detail::PostgresDialect dialect;
        detail::append_where(sql, parameters, defaults, query_spec.predicates(),
                             detail::lab_column_name, dialect);
        detail::append_order_limit(sql, parameters, query_spec, detail::lab_column_name, dialect);

        try {
          const auto result = txn_.work().exec(sql, pg_bind_params(parameters));
          std::vector<core::Lab> results;
          for (pqxx::row_ref row : result) {
            results.push_back(read_lab(row));
          }
          return results;
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
      }

      void insert(const core::Lab& entity, const MutationContext& context) override {
        detail::validate_lab(entity);
        try {
          pqxx::params params;
          params.append(entity.id.to_string());
          params.append(entity.name);
          params.append(entity.contact);
          params.append(entity.created_at.unix_micros());
          params.append(entity.settings_json.dump());
          params.append(entity.is_phi_enabled);
          params.append(micros_or_null(entity.archived_at));
          txn_.work().exec("INSERT INTO labs (id, name, contact, created_at_micros, settings_json, "
                           "is_phi_enabled, archived_at_micros) "
                           "VALUES ($1, $2, $3, $4, $5::jsonb, $6, $7)",
                           params);
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
        txn_.note_mutation(std::string(EntityTraits<core::Lab>::entity_name()),
                           entity.id.to_string(), context, "insert", detail::audit_after(entity));
      }

      void update(const core::Lab& entity, const MutationContext& context) override {
        detail::validate_lab(entity);
        const auto before = find_by_id(entity.id);
        try {
          pqxx::params params;
          params.append(entity.id.to_string());
          params.append(entity.name);
          params.append(entity.contact);
          params.append(entity.created_at.unix_micros());
          params.append(entity.settings_json.dump());
          params.append(entity.is_phi_enabled);
          params.append(micros_or_null(entity.archived_at));
          const auto result =
              txn_.work().exec("UPDATE labs SET name = $2, contact = $3, created_at_micros = $4, "
                               "settings_json = $5::jsonb, is_phi_enabled = $6, "
                               "archived_at_micros = $7 WHERE id = $1",
                               params);
          if (result.affected_rows() == 0) {
            throw NotFound("lab not found");
          }
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
        txn_.note_mutation(std::string(EntityTraits<core::Lab>::entity_name()),
                           entity.id.to_string(), context, "update",
                           detail::audit_change(before, entity));
      }

      void soft_delete(const core::LabId& entity_id, const MutationContext& context) override {
        auto entity = find_by_id(entity_id);
        if (!entity.has_value()) {
          throw NotFound("lab not found");
        }
        entity->archived_at = now_timestamp();
        update(entity.value(), context);
      }

    private:
      PostgresTransaction& txn_;
    };

    constexpr const char* user_columns =
        "id, primary_email, display_name, status, created_at_micros, auth_bindings_json, "
        "totp_secret_enc, default_lab_id, authz_version";

    class UserRepository final : public IRepository<core::User> {
    public:
      explicit UserRepository(PostgresTransaction& txn) : txn_(txn) {}

      [[nodiscard]] std::optional<core::User> find_by_id(const core::UserId& entity_id) override {
        try {
          const auto result =
              txn_.work().exec(std::string("SELECT ") + user_columns + " FROM users WHERE id = $1",
                               pqxx::params{entity_id.to_string()});
          if (result.empty()) {
            return std::nullopt;
          }
          return read_user(result[0]);
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
      }

      [[nodiscard]] std::vector<core::User> query(const Query<core::User>& query_spec) override {
        std::string sql = std::string("SELECT ") + user_columns + " FROM users";
        std::vector<nlohmann::json> parameters;
        const auto defaults = query_spec.includes_tombstoned()
                                  ? std::vector<std::string>{}
                                  : std::vector<std::string>{"status != 'disabled'"};
        detail::PostgresDialect dialect;
        detail::append_where(sql, parameters, defaults, query_spec.predicates(),
                             detail::user_column_name, dialect);
        detail::append_order_limit(sql, parameters, query_spec, detail::user_column_name, dialect);

        try {
          const auto result = txn_.work().exec(sql, pg_bind_params(parameters));
          std::vector<core::User> results;
          for (pqxx::row_ref row : result) {
            results.push_back(read_user(row));
          }
          return results;
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
      }

      void insert(const core::User& entity, const MutationContext& context) override {
        detail::validate_user(entity);
        try {
          pqxx::params params;
          params.append(entity.id.to_string());
          params.append(entity.primary_email);
          params.append(entity.display_name);
          params.append(std::string(core::to_string(entity.status)));
          params.append(entity.created_at.unix_micros());
          params.append(entity.auth_bindings.dump());
          params.append(entity.totp_secret_enc);
          params.append(id_or_null(entity.default_lab_id));
          params.append(entity.authz_version);
          txn_.work().exec(
              "INSERT INTO users (id, primary_email, display_name, status, created_at_micros, "
              "auth_bindings_json, totp_secret_enc, default_lab_id, authz_version) "
              "VALUES ($1, $2, $3, $4, $5, $6::jsonb, $7, $8, $9)",
              params);
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
        txn_.note_mutation(std::string(EntityTraits<core::User>::entity_name()),
                           entity.id.to_string(), context, "insert", detail::audit_after(entity));
      }

      void update(const core::User& entity, const MutationContext& context) override {
        detail::validate_user(entity);
        const auto before = find_by_id(entity.id);
        try {
          pqxx::params params;
          params.append(entity.id.to_string());
          params.append(entity.primary_email);
          params.append(entity.display_name);
          params.append(std::string(core::to_string(entity.status)));
          params.append(entity.created_at.unix_micros());
          params.append(entity.auth_bindings.dump());
          params.append(entity.totp_secret_enc);
          params.append(id_or_null(entity.default_lab_id));
          // Never let a stale in-memory snapshot lower the epoch below what an
          // authz mutation already committed; GREATEST keeps it monotonic.
          params.append(entity.authz_version);
          const auto result = txn_.work().exec(
              "UPDATE users SET primary_email = $2, display_name = $3, status = $4, "
              "created_at_micros = $5, auth_bindings_json = $6::jsonb, totp_secret_enc = $7, "
              "default_lab_id = $8, authz_version = GREATEST(authz_version, $9) WHERE id = $1",
              params);
          if (result.affected_rows() == 0) {
            throw NotFound("user not found");
          }
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
        txn_.note_mutation(std::string(EntityTraits<core::User>::entity_name()),
                           entity.id.to_string(), context, "update",
                           detail::audit_change(before, entity));
      }

      void soft_delete(const core::UserId& entity_id, const MutationContext& context) override {
        auto entity = find_by_id(entity_id);
        if (!entity.has_value()) {
          throw NotFound("user not found");
        }
        entity->status = core::UserStatus::Disabled;
        update(entity.value(), context);
      }

    private:
      PostgresTransaction& txn_;
    };

    // Bump a single user's authorization epoch in the current transaction so a
    // membership add/revoke/role-change invalidates any cached SessionContext on
    // the next request (mirrors the SQLite detail::bump_authz_version_for_user).
    void bump_authz_version_for_user(PostgresTransaction& txn, const core::UserId& user_id) {
      txn.work().exec("UPDATE users SET authz_version = authz_version + 1 WHERE id = $1",
                      pqxx::params{user_id.to_string()});
    }

    constexpr const char* membership_columns =
        "user_id, lab_id, role_id, scope_filters_json, invited_by, joined_at_micros, "
        "revoked_at_micros";

    class LabMembershipRepository final : public IRepository<core::LabMembership> {
    public:
      explicit LabMembershipRepository(PostgresTransaction& txn) : txn_(txn) {}

      [[nodiscard]] std::optional<core::LabMembership>
      find_by_id(const core::LabMembershipId& entity_id) override {
        try {
          const auto result = txn_.work().exec(
              std::string("SELECT ") + membership_columns +
                  " FROM lab_memberships WHERE user_id = $1 AND lab_id = $2",
              pqxx::params{entity_id.user_id.to_string(), entity_id.lab_id.to_string()});
          if (result.empty()) {
            return std::nullopt;
          }
          return read_membership(result[0]);
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
      }

      [[nodiscard]] std::vector<core::LabMembership>
      query(const Query<core::LabMembership>& query_spec) override {
        std::string sql = std::string("SELECT ") + membership_columns + " FROM lab_memberships";
        std::vector<nlohmann::json> parameters;
        const auto defaults = query_spec.includes_tombstoned()
                                  ? std::vector<std::string>{}
                                  : std::vector<std::string>{"revoked_at_micros IS NULL"};
        detail::PostgresDialect dialect;
        detail::append_where(sql, parameters, defaults, query_spec.predicates(),
                             detail::membership_column_name, dialect);
        detail::append_order_limit(sql, parameters, query_spec, detail::membership_column_name,
                                   dialect);

        try {
          const auto result = txn_.work().exec(sql, pg_bind_params(parameters));
          std::vector<core::LabMembership> results;
          for (pqxx::row_ref row : result) {
            results.push_back(read_membership(row));
          }
          return results;
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
      }

      void insert(const core::LabMembership& entity, const MutationContext& context) override {
        detail::validate_membership(entity);
        try {
          txn_.work().exec("INSERT INTO lab_memberships (user_id, lab_id, role_id, "
                           "scope_filters_json, invited_by, joined_at_micros, revoked_at_micros) "
                           "VALUES ($1, $2, $3, $4::jsonb, $5, $6, $7)",
                           bind_membership(entity));
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
        bump_authz_version_for_user(txn_, entity.user_id);
        txn_.note_mutation(std::string(EntityTraits<core::LabMembership>::entity_name()),
                           entity.id().to_string(), context, "insert", detail::audit_after(entity));
      }

      void update(const core::LabMembership& entity, const MutationContext& context) override {
        detail::validate_membership(entity);
        const auto before = find_by_id(entity.id());
        try {
          const auto result = txn_.work().exec(
              "UPDATE lab_memberships SET role_id = $3, scope_filters_json = $4::jsonb, "
              "invited_by = $5, joined_at_micros = $6, revoked_at_micros = $7 "
              "WHERE user_id = $1 AND lab_id = $2",
              bind_membership(entity));
          if (result.affected_rows() == 0) {
            throw NotFound("lab membership not found");
          }
        } catch (const pqxx::sql_error& err) {
          throw_pqxx_error(err);
        }
        bump_authz_version_for_user(txn_, entity.user_id);
        txn_.note_mutation(std::string(EntityTraits<core::LabMembership>::entity_name()),
                           entity.id().to_string(), context, "update",
                           detail::audit_change(before, entity));
      }

      void soft_delete(const core::LabMembershipId& entity_id,
                       const MutationContext& context) override {
        auto entity = find_by_id(entity_id);
        if (!entity.has_value()) {
          throw NotFound("lab membership not found");
        }
        entity->revoked_at = now_timestamp();
        update(entity.value(), context);
      }

    private:
      [[nodiscard]] static pqxx::params bind_membership(const core::LabMembership& entity) {
        pqxx::params params;
        params.append(entity.user_id.to_string());
        params.append(entity.lab_id.to_string());
        params.append(id_or_null(entity.role_id));
        params.append(entity.scope_filters_json.dump());
        params.append(id_or_null(entity.invited_by));
        params.append(entity.joined_at.unix_micros());
        params.append(micros_or_null(entity.revoked_at));
        return params;
      }

      PostgresTransaction& txn_;
    };

  } // namespace

  void register_identity_repositories(PostgresBackend& backend) {
    backend.register_repository_factory<core::Lab>(
        [](PostgresTransaction& txn) { return std::make_unique<LabRepository>(txn); });
    backend.register_repository_factory<core::User>(
        [](PostgresTransaction& txn) { return std::make_unique<UserRepository>(txn); });
    backend.register_repository_factory<core::LabMembership>(
        [](PostgresTransaction& txn) { return std::make_unique<LabMembershipRepository>(txn); });
  }

} // namespace fmgr::storage
