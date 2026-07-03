// SPDX-License-Identifier: AGPL-3.0-or-later

#include "storage/postgres/PostgresBackend.h"

#include "audit/AuditEventContent.h"
#include "audit/CanonicalJson.h"
#include "core/timestamp.h"
#include "storage/postgres/PostgresMigrationsEmbedded.h"

#include <pqxx/pqxx>
#include <sodium.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace fmgr::storage {
  namespace {

    // ---- Error mapping ----

    [[noreturn]] void throw_pqxx_error(const pqxx::sql_error& error) {
      const std::string_view state = error.sqlstate();
      if (state == "23505") {
        throw UniqueViolation(error.what());
      }
      if (state == "23503") {
        throw ForeignKeyViolation(error.what());
      }
      if (state == "23514" || state == "23502" || state == "23000") {
        throw ConstraintViolation(error.what());
      }
      if (state == "40001" || state == "40P01") {
        throw SerializationFailure(error.what());
      }
      // Connection-class errors (08xxx) and admin shutdown (57P01) → Unavailable.
      if (state.size() >= 2 && state.substr(0, 2) == "08") {
        throw Unavailable(error.what());
      }
      if (state == "57P01") {
        throw Unavailable(error.what());
      }
      // Unrecognised SQLSTATE — fall back to ConstraintViolation (safest for data-layer errors).
      throw ConstraintViolation(error.what());
    }

    // ---- UUID generation ----

    [[nodiscard]] std::string generate_random_uuid() {
      if (sodium_init() < 0) {
        throw std::runtime_error("libsodium initialisation failed");
      }
      std::array<unsigned char, 16> bytes{};
      randombytes_buf(bytes.data(), bytes.size());
      bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0fU) | 0x40U);
      bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3fU) | 0x80U);
      std::array<char, 37> buf{};
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
      std::snprintf(buf.data(), buf.size(),
                    "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                    bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
                    bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14],
                    bytes[15]);
      return {buf.data()};
    }

    // ---- Migrations ----

    [[nodiscard]] std::vector<PostgresMigration> default_migrations() {
      // SQL bodies live in src/storage/postgres/migrations/*.sql, embedded at
      // build time into PostgresMigrationsEmbedded.h (k_postgres_embedded_migrations).
      // Each migration is reviewable as its own file, and the checksum (blake2b
      // over "version:name:up_sql") stays byte-identical to the file content.
      std::vector<PostgresMigration> migrations;
      migrations.reserve(k_postgres_embedded_migrations.size());
      for (const auto& embedded : k_postgres_embedded_migrations) {
        migrations.push_back(PostgresMigration{.version = embedded.version,
                                               .name = std::string(embedded.name),
                                               .up_sql = std::string(embedded.up_sql)});
      }
      return migrations;
    }

    // BLAKE2b-256 hex digest of `data` via libsodium. Stable across platforms/compilers.
    [[nodiscard]] std::string blake2b_hex(const std::string& data) {
      if (sodium_init() < 0) {
        throw std::runtime_error("libsodium initialisation failed");
      }
      std::array<unsigned char, crypto_generichash_BYTES> hash{};
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
      crypto_generichash(hash.data(), hash.size(),
                         reinterpret_cast<const unsigned char*>(data.data()), data.size(), nullptr,
                         0);
      std::string hex;
      hex.reserve(hash.size() * 2);
      static constexpr std::array<char, 16> k_nibbles = {'0', '1', '2', '3', '4', '5', '6', '7',
                                                         '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
      for (const unsigned char byte : hash) {
        hex += k_nibbles[byte >> 4U];
        hex += k_nibbles[byte & 0x0fU];
      }
      return hex;
    }

    [[nodiscard]] std::string checksum(const PostgresMigration& migration) {
      return blake2b_hex(std::to_string(migration.version) + ":" + migration.name + ":" +
                         migration.up_sql);
    }

  } // namespace

  std::vector<PostgresMigration> default_postgres_migrations() {
    return default_migrations();
  }

  // ---- PostgresBackendState ----

  struct PostgresBackendState {
    explicit PostgresBackendState(PostgresBackendOptions input_options)
        : options(std::move(input_options)) {
      if (options.migrations.empty()) {
        options.migrations = default_migrations();
      }
      connections.reserve(options.pool_size);
      connection_available.assign(options.pool_size, true);
      for (std::size_t i = 0; i < options.pool_size; ++i) {
        connections.emplace_back(std::make_unique<pqxx::connection>(options.connection_string));
      }
    }

    [[nodiscard]] std::size_t acquire(std::chrono::milliseconds timeout) {
      std::unique_lock lock(pool_mutex);
      const bool found = pool_cv.wait_for(lock, timeout, [this] {
        return std::ranges::any_of(connection_available, [](bool available) { return available; });
      });
      if (!found) {
        throw Unavailable("postgres connection pool exhausted");
      }
      for (std::size_t i = 0; i < connection_available.size(); ++i) {
        if (connection_available[i]) {
          connection_available[i] = false;
          // Health-check: reconnect if the connection dropped (DB restart, idle timeout, etc.).
          if (!connections.at(i)->is_open()) {
            try {
              connections.at(i) = std::make_unique<pqxx::connection>(options.connection_string);
            } catch (const std::exception&) {
              connection_available[i] = true; // return to pool on reconnect failure
              pool_cv.notify_one();
              throw Unavailable("postgres reconnect failed");
            }
          }
          return i;
        }
      }
      throw Unavailable("postgres connection pool exhausted");
    }

    void release(std::size_t idx) {
      {
        std::scoped_lock lock(pool_mutex);
        connection_available[idx] = true;
      }
      pool_cv.notify_one();
    }

    PostgresBackendOptions options;
    std::vector<std::unique_ptr<pqxx::connection>> connections;
    std::vector<bool> connection_available;
    std::mutex pool_mutex;
    std::condition_variable pool_cv;
    std::mutex audit_mutex;
    bool fail_next_audit_append{false};
  };

  // ---- PostgresTransaction::Impl ----

  class PostgresTransaction::Impl {
  public:
    Impl(std::shared_ptr<PostgresBackendState> input_state, IsolationLevel input_isolation,
         std::size_t input_conn_idx)
        : state(std::move(input_state)), isolation(input_isolation), conn_idx(input_conn_idx) {
      // Emplace work referencing the acquired connection.
      work.emplace(*state->connections.at(conn_idx));
    }

    // Destroy the pqxx::work (rolls back if uncommitted) then release connection.
    ~Impl() {
      work.reset(); // abort if still active
      if (!released) {
        state->release(conn_idx);
        released = true;
      }
    }

    struct AuditMutation {
      std::string entity_kind;
      std::string entity_id;
      std::string action;
      MutationContext context;
      AuditSnapshot snapshot; // repository-derived before/after state
    };

    std::shared_ptr<PostgresBackendState> state;
    IsolationLevel isolation;
    std::size_t conn_idx;
    std::optional<pqxx::work> work; // reset explicitly before releasing connection
    std::vector<AuditMutation> audit_mutations;
    bool completed{false};
    bool released{false};
  };

  // ---- PostgresTransaction ----

  PostgresTransaction::PostgresTransaction(const std::shared_ptr<PostgresBackendState>& state,
                                           IsolationLevel isolation_level) {
    const std::size_t idx = state->acquire(state->options.pool_acquire_timeout);
    // Pass state by copy so that if Impl ctor throws, the outer 'state' is still valid
    // and we can release the acquired connection manually.
    try {
      impl_ = std::make_unique<Impl>(state, isolation_level, idx);
    } catch (...) {
      state->release(idx);
      throw;
    }
    // impl_ is live; its destructor will release the connection on further exceptions.
    try {
      if (isolation_level == IsolationLevel::Serializable) {
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        impl_->work->exec("SET TRANSACTION ISOLATION LEVEL SERIALIZABLE");
      } else if (isolation_level == IsolationLevel::RepeatableRead) {
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        impl_->work->exec("SET TRANSACTION ISOLATION LEVEL REPEATABLE READ");
      }
    } catch (const pqxx::sql_error& err) {
      throw_pqxx_error(err);
    }
  }

  PostgresTransaction::~PostgresTransaction() = default;

  pqxx::work& PostgresTransaction::work() {
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    return *impl_->work;
  }

  void PostgresTransaction::set_session_var(std::string_view key, std::string_view value) {
    try {
      // set_config(name, value, is_local=true) scopes to the current transaction.
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
      impl_->work->exec("SELECT set_config($1, $2, true)",
                        pqxx::params{"app." + std::string(key), std::string(value)});
    } catch (const pqxx::sql_error& err) {
      throw_pqxx_error(err);
    }
  }

  void PostgresTransaction::note_mutation(std::string entity_kind, std::string entity_id,
                                          const MutationContext& context, std::string action,
                                          AuditSnapshot snapshot) {
    impl_->audit_mutations.push_back(Impl::AuditMutation{
        .entity_kind = std::move(entity_kind),
        .entity_id = std::move(entity_id),
        .action = std::move(action),
        .context = context,
        .snapshot = std::move(snapshot),
    });
  }

  void PostgresTransaction::note_phi_read(const std::string& entity_kind,
                                          const std::string& entity_id,
                                          const MutationContext& context,
                                          const std::vector<std::string>& field_keys) {
    // Records key names only; the disclosed values never enter the audit chain.
    note_mutation(
        entity_kind, entity_id, context, "phi.read",
        AuditSnapshot{.before = std::nullopt, .after = nlohmann::json{{"phi_keys", field_keys}}});
  }

  void PostgresTransaction::commit() {
    if (impl_->completed) {
      throw ConstraintViolation("postgres transaction already completed");
    }
    impl_->completed = true;

    try {
      {
        std::scoped_lock lock(impl_->state->audit_mutex);
        if (impl_->state->fail_next_audit_append && !impl_->audit_mutations.empty()) {
          impl_->state->fail_next_audit_append = false;
          throw ConstraintViolation("audit append failed");
        }
      }

      if (!impl_->audit_mutations.empty()) {
        if (sodium_init() < 0) {
          throw ConstraintViolation("libsodium initialisation failed");
        }

        // Advisory lock serialises concurrent audit appends within this DB.
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        impl_->work->exec("SELECT pg_advisory_xact_lock(8675309)");

        // The chain tail is the row whose this_hash no other row links from. Rows
        // in one commit share at_micros and have random ids, so an ORDER BY on
        // those cannot recover the true tail; the link structure can. The
        // advisory lock above serialises appenders, so exactly one tail exists.
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        const auto last = impl_->work->exec(
            "SELECT e.this_hash FROM audit_events e "
            "WHERE NOT EXISTS (SELECT 1 FROM audit_events p WHERE p.prev_hash = e.this_hash)");
        std::string prev_hash =
            last.empty() ? std::string(audit::zero_hash()) : last[0][0].as<std::string>();

        const auto now_micros =
            static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());

        for (const auto& mutation : impl_->audit_mutations) {
          const auto event_id = generate_random_uuid();
          const std::string before_str =
              mutation.snapshot.before.has_value() ? mutation.snapshot.before->dump() : "{}";
          const std::string after_str =
              mutation.snapshot.after.has_value() ? mutation.snapshot.after->dump() : "{}";
          const auto content_json = audit::audit_event_content_json(audit::AuditEventContentFields{
              .action = mutation.action,
              .actor_session_id = mutation.context.actor_session_id,
              .actor_user_id = mutation.context.actor_user_id.to_string(),
              .after_json = after_str,
              .at_micros = now_micros,
              .before_json = before_str,
              .entity_id = mutation.entity_id,
              .entity_kind = mutation.entity_kind,
              .id = event_id,
              .lab_id = mutation.context.lab_id,
              .request_id = mutation.context.request_id,
          });
          const auto this_hash = audit::compute_audit_hash(prev_hash, content_json);

          pqxx::params audit_params;
          audit_params.append(event_id);
          audit_params.append(now_micros);
          audit_params.append(mutation.context.actor_user_id.to_string());
          audit_params.append(mutation.context.actor_session_id);
          audit_params.append(mutation.context.lab_id); // optional: binds NULL if empty
          audit_params.append(mutation.action);
          audit_params.append(mutation.entity_kind);
          audit_params.append(mutation.entity_id);
          audit_params.append(before_str);
          audit_params.append(after_str);
          audit_params.append(mutation.context.request_id);
          audit_params.append(prev_hash);
          audit_params.append(this_hash);
          // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
          impl_->work->exec(
              "INSERT INTO audit_events "
              "(id,at_micros,actor_user_id,actor_session_id,lab_id,action,"
              "entity_kind,entity_id,before_json,after_json,request_id,prev_hash,this_hash) "
              "VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13)",
              audit_params);

          prev_hash = this_hash;
        }
      }

      // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
      impl_->work->commit();
    } catch (const pqxx::sql_error& err) {
      impl_->work.reset();
      impl_->state->release(impl_->conn_idx);
      impl_->released = true;
      throw_pqxx_error(err);
    } catch (...) {
      impl_->work.reset();
      impl_->state->release(impl_->conn_idx);
      impl_->released = true;
      throw;
    }

    impl_->work.reset();
    impl_->state->release(impl_->conn_idx);
    impl_->released = true;
  }

  void PostgresTransaction::rollback() {
    if (!impl_->completed) {
      impl_->completed = true;
      impl_->work.reset(); // pqxx::work destructor calls abort()
      impl_->state->release(impl_->conn_idx);
      impl_->released = true;
    }
  }

  // ---- PostgresBackend ----

  PostgresBackend::PostgresBackend(PostgresBackendOptions options)
      : state_(std::make_shared<PostgresBackendState>(std::move(options))) {}

  PostgresBackend::~PostgresBackend() = default;
  PostgresBackend::PostgresBackend(PostgresBackend&&) noexcept = default;
  PostgresBackend& PostgresBackend::operator=(PostgresBackend&&) noexcept = default;

  void PostgresBackend::migrate_to_latest() {
    const std::size_t idx = state_->acquire(state_->options.pool_acquire_timeout);
    auto& conn = *state_->connections.at(idx);
    try {
      pqxx::work txn(conn);
      txn.exec(R"sql(
CREATE TABLE IF NOT EXISTS schema_migrations (
  version                 INTEGER PRIMARY KEY,
  name                    TEXT    NOT NULL,
  checksum                TEXT    NOT NULL,
  applied_at_unix_seconds BIGINT  NOT NULL
))sql");

      auto migrations = state_->options.migrations;
      std::ranges::sort(migrations, {}, &PostgresMigration::version);

      int previous_version = 0;
      for (const auto& migration : migrations) {
        if (migration.version <= previous_version) {
          throw MigrationFailure("postgres migration versions must be strictly increasing");
        }
        previous_version = migration.version;

        const auto expected_cs = checksum(migration);
        const auto cs_result = txn.exec("SELECT checksum FROM schema_migrations WHERE version = $1",
                                        pqxx::params{migration.version});

        if (!cs_result.empty()) {
          const auto applied_cs = cs_result[0][0].as<std::string>();
          if (applied_cs != expected_cs) {
            throw MigrationFailure("postgres migration checksum changed: " + migration.name);
          }
          continue;
        }

        txn.exec(migration.up_sql);
        const auto applied_at =
            static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
        txn.exec("INSERT INTO schema_migrations "
                 "(version,name,checksum,applied_at_unix_seconds) VALUES ($1,$2,$3,$4)",
                 pqxx::params{migration.version, migration.name, expected_cs, applied_at});
      }
      txn.commit();
    } catch (const pqxx::sql_error& err) {
      state_->release(idx);
      throw MigrationFailure(err.what());
    } catch (const BackendError&) {
      state_->release(idx);
      throw;
    } catch (const std::exception& err) {
      state_->release(idx);
      throw MigrationFailure(err.what());
    }
    state_->release(idx);
  }

  SchemaVersion PostgresBackend::current_version() const {
    const std::size_t idx = state_->acquire(state_->options.pool_acquire_timeout);
    auto& conn = *state_->connections.at(idx);
    int version = 0;
    try {
      pqxx::work txn(conn);
      const auto result = txn.exec("SELECT COALESCE(MAX(version),0) FROM schema_migrations");
      if (!result.empty()) {
        version = result[0][0].as<int>();
      }
      txn.commit();
    } catch (const pqxx::sql_error& err) {
      if (std::string_view(err.sqlstate()) != "42P01") {
        // 42P01 = "undefined_table" (schema_migrations not yet created) — treat as version 0.
        // Any other error (permissions denied, connection lost, etc.) is a real failure.
        state_->release(idx);
        throw Unavailable(err.what());
      }
    }
    state_->release(idx);
    return SchemaVersion{version};
  }

  std::unique_ptr<ITransaction> PostgresBackend::begin(IsolationLevel isolation_level) {
    auto txn =
        std::unique_ptr<PostgresTransaction>(new PostgresTransaction(state_, isolation_level));
    for (const auto& [unused_type, factory] : repository_factories_) {
      (void)unused_type;
      factory(*txn);
    }
    return txn;
  }

  Capabilities PostgresBackend::caps() const {
    Capabilities result;
    result.row_level_security = true;
    result.json_path_equality = true;
    result.json_path_indexes = true;
    result.native_uuid = false;
    result.listen_notify = true;
    return result;
  }

  void PostgresBackend::fail_next_audit_append_for_tests() {
    std::scoped_lock lock(state_->audit_mutex);
    state_->fail_next_audit_append = true;
  }

  std::size_t PostgresBackend::audit_event_count_for_tests() const {
    const std::size_t idx = state_->acquire(state_->options.pool_acquire_timeout);
    auto& conn = *state_->connections.at(idx);
    std::size_t count = 0;
    try {
      pqxx::work txn(conn);
      const auto result = txn.exec("SELECT COUNT(*) FROM audit_events");
      if (!result.empty()) {
        count = result[0][0].as<std::size_t>();
      }
      txn.commit();
      // NOLINTNEXTLINE(bugprone-empty-catch)
    } catch (const pqxx::sql_error&) {
      // audit_events doesn't exist yet — return 0
    }
    state_->release(idx);
    return count;
  }

  void PostgresBackend::downgrade_to_zero_for_tests() {
    const std::size_t idx = state_->acquire(state_->options.pool_acquire_timeout);
    auto& conn = *state_->connections.at(idx);
    try {
      pqxx::work txn(conn);
      txn.exec("DELETE FROM schema_migrations");
      txn.commit();
    } catch (const pqxx::sql_error& err) {
      state_->release(idx);
      throw BackendError(BackendErrorCode::ConstraintViolation, err.what());
    }
    state_->release(idx);
  }

} // namespace fmgr::storage
