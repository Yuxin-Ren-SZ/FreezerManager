// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_STORAGE_POSTGRES_POSTGRESBACKEND_H
#define FMGR_STORAGE_POSTGRES_POSTGRESBACKEND_H

#include "storage/IStorageBackend.h"

#include <pqxx/pqxx>

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <typeindex>
#include <utility>
#include <vector>

namespace fmgr::storage {

  struct PostgresMigration {
    int version{0};
    std::string name;
    std::string up_sql;
  };

  struct PostgresBackendOptions {
    std::string connection_string;
    std::size_t pool_size{5};
    std::chrono::milliseconds pool_acquire_timeout{5000};
    std::vector<PostgresMigration> migrations;
  };

  // The production Postgres schema as an ordered migration list. Exposed so tests
  // can build on the real schema instead of duplicating table DDL.
  [[nodiscard]] std::vector<PostgresMigration> default_postgres_migrations();

  struct PostgresBackendState;

  class PostgresTransaction final : public ITransaction {
  public:
    ~PostgresTransaction() override;
    PostgresTransaction(const PostgresTransaction&) = delete;
    PostgresTransaction& operator=(const PostgresTransaction&) = delete;
    PostgresTransaction(PostgresTransaction&&) = delete;
    PostgresTransaction& operator=(PostgresTransaction&&) = delete;

    // Set a Postgres session variable scoped to this transaction (for RLS).
    // Uses set_config(key, value, is_local=true) to avoid injection.
    void set_session_var(std::string_view key, std::string_view value) override;

    // Record a mutation for the audit chain written at commit time.
    void note_mutation(std::string entity_kind, std::string entity_id,
                       const MutationContext& context, std::string action = "mutation",
                       AuditSnapshot snapshot = {});

    void note_phi_read(const std::string& entity_kind, const std::string& entity_id,
                       const MutationContext& context,
                       const std::vector<std::string>& field_keys) override;

    [[nodiscard]] pqxx::work& work();

    template <typename Entity>
    void register_postgres_repository(std::unique_ptr<IRepository<Entity>> repository) {
      register_repository<Entity>(std::move(repository));
    }

    void commit() override;
    void rollback() override;

  private:
    friend class PostgresBackend;
    PostgresTransaction(const std::shared_ptr<PostgresBackendState>& state,
                        IsolationLevel isolation_level);
    class Impl;
    std::unique_ptr<Impl> impl_;
  };

  class PostgresBackend final : public IStorageBackend {
  public:
    explicit PostgresBackend(PostgresBackendOptions options);
    ~PostgresBackend() override;
    PostgresBackend(const PostgresBackend&) = delete;
    PostgresBackend& operator=(const PostgresBackend&) = delete;
    PostgresBackend(PostgresBackend&&) noexcept;
    PostgresBackend& operator=(PostgresBackend&&) noexcept;

    template <typename Entity, typename Factory> void register_repository_factory(Factory factory) {
      repository_factories_.insert_or_assign(
          std::type_index(typeid(Entity)),
          [factory = std::move(factory)](PostgresTransaction& txn) mutable {
            txn.register_postgres_repository<Entity>(factory(txn));
          });
    }

    void migrate_to_latest() override;
    [[nodiscard]] SchemaVersion current_version() const override;
    [[nodiscard]] std::unique_ptr<ITransaction> begin(IsolationLevel isolation_level) override;
    [[nodiscard]] Capabilities caps() const override;

    // Test-only hooks mirroring the SQLite backend interface.
    void fail_next_audit_append_for_tests();
    [[nodiscard]] std::size_t audit_event_count_for_tests() const;
    void downgrade_to_zero_for_tests();

  private:
    std::shared_ptr<PostgresBackendState> state_;
    std::map<std::type_index, std::function<void(PostgresTransaction&)>> repository_factories_;
  };

} // namespace fmgr::storage

#endif // FMGR_STORAGE_POSTGRES_POSTGRESBACKEND_H
