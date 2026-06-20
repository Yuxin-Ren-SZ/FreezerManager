// SPDX-License-Identifier: AGPL-3.0-or-later

#include "backup/BackupCommands.h"

#include "audit/AuditChainVerifier.h"
#include "backup/SqliteBackup.h"
#include "core/audit_event.h"
#include "core/timestamp.h"
#include "crypto/FieldCipher.h" // crypto::CipherError
#include "crypto/FileCipher.h"
#include "storage/AuditTraits.h"
#include "storage/sqlite/AuditRepositories.h"
#include "storage/sqlite/SqliteBackend.h"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace fmgr::backup {

  namespace {

    std::int64_t now_micros() {
      return std::chrono::duration_cast<std::chrono::microseconds>(
                 std::chrono::system_clock::now().time_since_epoch())
          .count();
    }

    int read_schema_version(const std::string& db_path) {
      storage::SqliteBackend backend(storage::SqliteBackendOptions{.database_path = db_path});
      return backend.current_version().value;
    }

    // Open a SQLite database with only the read-only audit-event repository
    // registered — enough to append a `backup.*` event (the chain is written by
    // commit()) and to read the chain back for verification. We deliberately do
    // not register the full domain repositories or run migrations: a restored
    // backup is already at its own schema version, and the backup engines never
    // touch domain rows.
    std::unique_ptr<storage::SqliteBackend> open_sqlite_audit(const std::string& db_path) {
      auto backend = std::make_unique<storage::SqliteBackend>(
          storage::SqliteBackendOptions{.database_path = db_path});
      storage::register_audit_repositories(*backend);
      return backend;
    }

    // Remove a SQLite database file plus any WAL/SHM/journal sidecars.
    void remove_db_files(const std::filesystem::path& path) {
      std::error_code error;
      for (const char* suffix : {"", "-wal", "-shm", "-journal"}) {
        std::filesystem::remove(std::filesystem::path(path.string() + suffix), error);
      }
    }

    bool integrity_check(const std::string& db_path, std::string& detail) {
      sqlite3* conn = nullptr;
      if (sqlite3_open_v2(db_path.c_str(), &conn, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        detail = "cannot open restored database";
        sqlite3_close(conn);
        return false;
      }
      sqlite3_stmt* stmt = nullptr;
      bool passed = false;
      if (sqlite3_prepare_v2(conn, "PRAGMA integrity_check", -1, &stmt, nullptr) == SQLITE_OK &&
          sqlite3_step(stmt) == SQLITE_ROW) {
        const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const std::string result = (text != nullptr) ? text : "";
        passed = (result == "ok");
        if (!passed) {
          detail = "integrity_check: " + result;
        }
      } else {
        detail = "integrity_check did not run";
      }
      sqlite3_finalize(stmt);
      sqlite3_close(conn);
      return passed;
    }

    [[nodiscard]] std::string_view chain_status_label(audit::AuditChainStatus status) {
      switch (status) {
      case audit::AuditChainStatus::BrokenLink:
        return "broken link";
      case audit::AuditChainStatus::HashMismatch:
        return "hash mismatch (row tampered)";
      case audit::AuditChainStatus::Ok:
        return "ok";
      }
      return "unknown";
    }

    // Walk the audit chain of an already-opened SQLite database. Returns 0 if
    // intact, 1 on the first divergence (details written to `out`). Self-contained
    // so the backup library does not depend on the CLI's `audit verify` command.
    int verify_audit_chain(storage::SqliteBackend& backend, std::ostream& out) {
      auto txn = backend.begin(storage::IsolationLevel::Serializable);
      const auto events = txn->repo<core::AuditEvent>().query(
          storage::Query<core::AuditEvent>::all()
              .order_by(
                  storage::field<core::AuditEvent, core::Timestamp>(core::AuditEvent::Field::At),
                  storage::SortDirection::Ascending)
              .order_by(storage::field<core::AuditEvent, std::string>(core::AuditEvent::Field::Id),
                        storage::SortDirection::Ascending));
      txn->commit();

      const auto report = audit::verify_audit_chain(events);
      if (report.ok || !report.first_error.has_value()) {
        out << "OK: " << report.verified_count << " audit event(s) verified\n";
        return 0;
      }
      const auto& error = report.first_error.value();
      out << "DIVERGENCE at #" << error.index << " (id=" << error.id.to_string()
          << "): " << chain_status_label(error.status) << " — " << error.detail << '\n';
      out << report.verified_count << " event(s) verified before the divergence\n";
      return 1;
    }

  } // namespace

  void append_backup_event(storage::IStorageBackend& backend, const std::string& action,
                           const std::string& entity_id, const nlohmann::json& after,
                           core::UserId actor) {
    auto txn = backend.begin(storage::IsolationLevel::Serializable);
    auto* sqlite_txn = dynamic_cast<storage::SqliteTransaction*>(txn.get());
    if (sqlite_txn == nullptr) {
      throw BackupError("backup auditing requires a SQLite backend in this release");
    }
    const storage::MutationContext ctx{
        .actor_user_id = actor,
        .actor_session_id = "freezerctl",
        .request_id = "",
        .reason = action,
        .lab_id = std::nullopt,
    };
    sqlite_txn->note_mutation("backup", entity_id, ctx, action,
                              storage::AuditSnapshot{.before = std::nullopt, .after = after});
    txn->commit();
  }

  BackupCreateReport run_backup_create(storage::IStorageBackend& backend,
                                       const std::string& sqlite_db_path,
                                       const kms::IKmsProvider& backup_kms,
                                       const std::string& out_path, core::UserId actor,
                                       std::ostream& sink) {
    const std::filesystem::path temp = std::filesystem::path(out_path + ".hotcopy.tmp");
    remove_db_files(temp);
    hot_copy(sqlite_db_path, temp.string());

    const int schema_version = read_schema_version(temp.string());
    const crypto::BackupManifest manifest =
        crypto::encrypt_file(temp, std::filesystem::path(out_path), backup_kms,
                             crypto::FileEnvelopeMeta{.schema_version = schema_version,
                                                      .backend = "sqlite",
                                                      .created_at_micros = now_micros()});
    remove_db_files(temp);

    const std::string filename = std::filesystem::path(out_path).filename().string();
    append_backup_event(backend, "backup.create", filename,
                        nlohmann::json{{"path", out_path},
                                       {"schema_version", schema_version},
                                       {"content_sha256", manifest.content_sha256}},
                        actor);

    sink << "wrote encrypted backup " << out_path << " (schema v" << schema_version << ", sha256 "
         << manifest.content_sha256 << ")\n";
    return BackupCreateReport{.out_path = out_path,
                              .schema_version = schema_version,
                              .content_sha256 = manifest.content_sha256};
  }

  BackupVerifyReport run_backup_verify(const std::string& in_path,
                                       const kms::IKmsProvider& backup_kms, std::ostream& sink) {
    BackupVerifyReport report;
    const std::filesystem::path temp = std::filesystem::path(in_path + ".verify.tmp.db");
    remove_db_files(temp);

    crypto::BackupManifest manifest;
    try {
      manifest = crypto::decrypt_file(std::filesystem::path(in_path), temp, backup_kms);
    } catch (const crypto::CipherError& error) {
      remove_db_files(temp);
      report.detail = std::string("decrypt failed: ") + error.what();
      sink << "FAIL: " << report.detail << '\n';
      return report;
    }
    report.schema_version = manifest.schema_version;

    if (!integrity_check(temp.string(), report.detail)) {
      remove_db_files(temp);
      sink << "FAIL: " << report.detail << '\n';
      return report;
    }

    std::ostringstream audit_out;
    int audit_rc = 0;
    {
      auto backend = open_sqlite_audit(temp.string());
      audit_rc = verify_audit_chain(*backend, audit_out);
    }
    remove_db_files(temp);
    if (audit_rc != 0) {
      report.detail = "audit chain: " + audit_out.str();
      sink << "FAIL: " << report.detail << '\n';
      return report;
    }

    report.ok = true;
    report.detail = audit_out.str();
    sink << "PASS: backup is restorable (schema v" << manifest.schema_version << ", "
         << report.detail;
    if (!report.detail.empty() && report.detail.back() != '\n') {
      sink << '\n';
    }
    return report;
  }

  BackupRestoreReport run_backup_restore(const std::string& in_path,
                                         const kms::IKmsProvider& backup_kms,
                                         const std::string& out_path, bool force,
                                         core::UserId actor, std::ostream& sink) {
    std::error_code error;
    if (std::filesystem::exists(std::filesystem::path(out_path), error) && !force) {
      throw BackupError("refusing to overwrite existing file: " + out_path + " (use --force)");
    }

    const std::filesystem::path temp = std::filesystem::path(out_path + ".restore.tmp.db");
    remove_db_files(temp);
    const crypto::BackupManifest manifest =
        crypto::decrypt_file(std::filesystem::path(in_path), temp, backup_kms);

    remove_db_files(std::filesystem::path(out_path));
    std::filesystem::rename(temp, std::filesystem::path(out_path), error);
    if (error) {
      remove_db_files(temp);
      throw BackupError("cannot move restored database into place: " + error.message());
    }

    // Record the restoration in the restored database's own audit chain.
    {
      auto backend = open_sqlite_audit(out_path);
      append_backup_event(*backend, "backup.restore",
                          std::filesystem::path(in_path).filename().string(),
                          nlohmann::json{{"source", in_path},
                                         {"restored_to", out_path},
                                         {"schema_version", manifest.schema_version}},
                          actor);
    }

    sink << "restored " << out_path << " from " << in_path << " (schema v"
         << manifest.schema_version << ")\n";
    return BackupRestoreReport{.out_path = out_path, .schema_version = manifest.schema_version};
  }

} // namespace fmgr::backup
