// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_STORAGE_SQLITE_SQLITEBACKEND_H
#define FMGR_STORAGE_SQLITE_SQLITEBACKEND_H

#include "storage/IStorageBackend.h"

#include <sqlite3.h>

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <typeindex>
#include <utility>
#include <vector>

namespace fmgr::storage {

  struct SqliteMigration {
    int version{0};
    std::string name;
    std::string up_sql;
  };

  struct SqliteBackendOptions {
    std::string database_path{":memory:"};
    std::chrono::milliseconds busy_timeout{5000};
    std::vector<SqliteMigration> migrations;
  };

  struct SqliteBackendState;

  class SqliteTransaction final : public ITransaction {
  public:
    ~SqliteTransaction() override;

    SqliteTransaction(const SqliteTransaction&) = delete;
    SqliteTransaction& operator=(const SqliteTransaction&) = delete;
    SqliteTransaction(SqliteTransaction&&) = delete;
    SqliteTransaction& operator=(SqliteTransaction&&) = delete;

    [[nodiscard]] sqlite3* handle() const;
    [[nodiscard]] IsolationLevel isolation_level() const;

    void add_commit_hook(std::function<void(sqlite3*)> hook);
    // Record a mutation for inclusion in the audit chain written at commit time.
    // action defaults to "mutation"; repos that know the specific operation
    // (insert/update/soft_delete) may pass it explicitly.
    void note_mutation(std::string entity_kind, std::string entity_id,
                       const MutationContext& context, std::string action = "mutation");

    template <typename Entity>
    void register_sqlite_repository(std::unique_ptr<IRepository<Entity>> repository) {
      register_repository<Entity>(std::move(repository));
    }

    void commit() override;
    void rollback() override;

  private:
    friend class SqliteBackend;

    SqliteTransaction(std::shared_ptr<SqliteBackendState> state, IsolationLevel isolation_level);

    class Impl;
    std::unique_ptr<Impl> impl_;
  };

  class SqliteBackend final : public IStorageBackend {
  public:
    explicit SqliteBackend(SqliteBackendOptions options);
    ~SqliteBackend() override;

    SqliteBackend(const SqliteBackend&) = delete;
    SqliteBackend& operator=(const SqliteBackend&) = delete;
    SqliteBackend(SqliteBackend&&) noexcept;
    SqliteBackend& operator=(SqliteBackend&&) noexcept;

    template <typename Entity, typename Factory> void register_repository_factory(Factory factory) {
      repository_factories_.insert_or_assign(
          std::type_index(typeid(Entity)),
          [factory = std::move(factory)](SqliteTransaction& transaction) mutable {
            transaction.register_sqlite_repository<Entity>(factory(transaction));
          });
    }

    void migrate_to_latest() override;
    [[nodiscard]] SchemaVersion current_version() const override;
    [[nodiscard]] std::unique_ptr<ITransaction> begin(IsolationLevel isolation_level) override;
    [[nodiscard]] Capabilities caps() const override;

    void fail_next_audit_append_for_tests();
    [[nodiscard]] std::size_t audit_event_count_for_tests() const;
    void downgrade_to_zero_for_tests();

  private:
    std::shared_ptr<SqliteBackendState> state_;
    std::map<std::type_index, std::function<void(SqliteTransaction&)>> repository_factories_;
  };

} // namespace fmgr::storage

#endif // FMGR_STORAGE_SQLITE_SQLITEBACKEND_H
