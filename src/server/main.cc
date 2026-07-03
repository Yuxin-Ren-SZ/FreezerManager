// SPDX-License-Identifier: AGPL-3.0-or-later

#include "auth/LocalAuthProvider.h"
#include "backup/BackupRunner.h"
#include "core/ids.h"
#include "kms/IKmsProvider.h"
#include "kms/KmsFactory.h"
#include "obs/Health.h"
#include "obs/Log.h"
#include "rest/GatewayStubs.h"
#include "rest/RestGateway.h"
#include "server/FreezerServer.h"
#include "storage/sqlite/AuditRepositories.h"
#include "storage/sqlite/BoxGeometryRepositories.h"
#include "storage/sqlite/IdentityRepositories.h"
#include "storage/sqlite/ItemTypeRepositories.h"
#include "storage/sqlite/LayoutRepositories.h"
#include "storage/sqlite/LoginAttemptRepositories.h"
#include "storage/sqlite/RoleRepositories.h"
#include "storage/sqlite/SampleRepositories.h"
#include "storage/sqlite/SessionRepositories.h"
#include "storage/sqlite/ShareRequestRepositories.h"
#include "storage/sqlite/SqliteBackend.h"

#include <drogon/drogon.h>
#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

  // Split "host:port" on the last colon (IPv6-safe enough for the dev default).
  struct HostPort {
    std::string host;
    std::uint16_t port;
  };

  [[nodiscard]] HostPort parse_listen(const std::string& addr, std::uint16_t default_port) {
    const auto colon = addr.rfind(':');
    if (colon == std::string::npos) {
      return {addr, default_port};
    }
    return {addr.substr(0, colon), static_cast<std::uint16_t>(std::stoi(addr.substr(colon + 1)))};
  }

  // Database readiness: open and immediately roll back a transaction. A reachable
  // backend completes this cheaply; an unreachable one throws.
  [[nodiscard]] fmgr::obs::DepStatus probe_database(fmgr::storage::IStorageBackend& backend) {
    try {
      auto txn = backend.begin(fmgr::storage::IsolationLevel::ReadCommitted);
      txn->rollback();
      return fmgr::obs::DepStatus::ok();
    } catch (const std::exception& e) {
      return fmgr::obs::DepStatus::failed(e.what());
    }
  }

  // KMS readiness: a wrap → unwrap round-trip under the active KEK. Proves the key
  // is present AND usable, not merely configured. A null provider means PHI
  // encryption is off (a valid deployment), reported as disabled.
  [[nodiscard]] fmgr::obs::DepStatus probe_kms(const fmgr::kms::IKmsProvider* kms) {
    if (kms == nullptr) {
      return fmgr::obs::DepStatus::disabled("no master KEK configured");
    }
    try {
      const std::array<std::uint8_t, 32> probe_dek{};
      const auto wrapped = kms->wrap_dek(probe_dek);
      const auto recovered = kms->unwrap_dek(wrapped, kms->key_id());
      if (recovered.size() != probe_dek.size() ||
          !std::equal(recovered.begin(), recovered.end(), probe_dek.begin())) {
        return fmgr::obs::DepStatus::failed("KEK round-trip mismatch");
      }
      return fmgr::obs::DepStatus::ok("key_id=" + kms->key_id());
    } catch (const std::exception& e) {
      return fmgr::obs::DepStatus::failed(e.what());
    }
  }

  // Backup-target readiness: the configured directory must exist and be writable.
  // No target configured is a valid deployment, reported as disabled.
  [[nodiscard]] fmgr::obs::DepStatus probe_backup(const std::string& backup_dir) {
    if (backup_dir.empty()) {
      return fmgr::obs::DepStatus::disabled("FMGR_BACKUP_DIR unset");
    }
    std::error_code error;
    if (!std::filesystem::is_directory(backup_dir, error)) {
      return fmgr::obs::DepStatus::failed("not a directory: " + backup_dir);
    }
    const auto probe_file = std::filesystem::path(backup_dir) / ".freezerd-health-probe";
    std::ofstream out(probe_file);
    if (!out) {
      return fmgr::obs::DepStatus::failed("backup directory not writable: " + backup_dir);
    }
    out.close();
    std::filesystem::remove(probe_file, error);
    return fmgr::obs::DepStatus::ok();
  }

  // Populate the gRPC TLS/mTLS options from the FMGR_TLS_* environment (PRD §6).
  // See doc/TLS.md for the full variable reference.
  void apply_tls_env(fmgr::server::FreezerServerOptions& opts) {
    if (const char* cert = std::getenv("FMGR_TLS_CERT"); cert != nullptr) {
      opts.tls_cert_path = cert;
    }
    if (const char* key = std::getenv("FMGR_TLS_KEY"); key != nullptr) {
      opts.tls_key_path = key;
    }
    const char* require_tls = std::getenv("FMGR_REQUIRE_TLS");
    opts.require_tls = require_tls != nullptr &&
                       (std::string(require_tls) == "1" || std::string(require_tls) == "true");

    if (const char* client_ca = std::getenv("FMGR_TLS_CLIENT_CA"); client_ca != nullptr) {
      opts.tls_client_ca_path = client_ca;
    }
    if (const char* mtls = std::getenv("FMGR_MTLS"); mtls != nullptr) {
      const std::string mode = mtls;
      if (mode == "require") {
        opts.mtls = fmgr::server::MtlsMode::Require;
      } else if (mode == "request") {
        opts.mtls = fmgr::server::MtlsMode::Request;
      } else if (mode == "off") {
        opts.mtls = fmgr::server::MtlsMode::Off;
      } else {
        throw std::invalid_argument("FMGR_MTLS must be one of: off, request, require");
      }
    }
    if (const char* reload = std::getenv("FMGR_TLS_RELOAD_SEC"); reload != nullptr) {
      opts.tls_reload_interval_sec = static_cast<unsigned>(std::stoul(reload));
    }
  }

} // namespace

// Minimal entry point: reads FMGR_LISTEN (default "0.0.0.0:50051") and
// FMGR_DB_PATH (default ":memory:") from the environment and starts the server
// with a SQLite backend. Full configuration via freezerd.toml is deferred.
int main(int /*argc*/, char* /*argv*/[]) {
  // Structured JSON logging (PRD §17): every line is one JSON object with a fixed
  // schema (ts/level/msg/request_id/actor_user_id/lab_id/event) on an async,
  // non-blocking sink so log writes never stall the gRPC thread pool.
  fmgr::obs::init_logging();

  try {
    const char* listen_env = std::getenv("FMGR_LISTEN");
    const std::string listen = listen_env != nullptr ? listen_env : "0.0.0.0:50051";

    const char* db_env = std::getenv("FMGR_DB_PATH");
    const std::string db_path = db_env != nullptr ? db_env : ":memory:";

    auto backend = std::make_unique<fmgr::storage::SqliteBackend>(
        fmgr::storage::SqliteBackendOptions{.database_path = db_path});
    // Register every domain repository before serving (mirrors the CLI's
    // BackendFactory). Without this, repo<Entity>() inside an RPC handler throws
    // "repository is not available for entity type" and every request 500s.
    fmgr::storage::register_identity_repositories(*backend);
    fmgr::storage::register_role_repositories(*backend);
    fmgr::storage::register_layout_repositories(*backend);
    fmgr::storage::register_box_geometry_repositories(*backend);
    fmgr::storage::register_box_repositories(*backend);
    fmgr::storage::register_item_type_repositories(*backend);
    fmgr::storage::register_sample_repositories(*backend);
    fmgr::storage::register_session_repositories(*backend);
    fmgr::storage::register_login_attempt_repositories(*backend);
    fmgr::storage::register_share_request_repositories(*backend);
    fmgr::storage::register_audit_repositories(*backend);
    backend->migrate_to_latest();

    // Master-KEK provider for TOTP-secret encryption (C-3). Null when no KEK is
    // configured: TOTP enrolment is then unavailable but password login still
    // works. Must outlive `auth`, which borrows it.
    auto auth_kms = fmgr::kms::make_default_kms();
    fmgr::auth::LocalAuthProvider auth(*backend, fmgr::auth::LocalAuthProviderConfig{},
                                       auth_kms.get());

    fmgr::server::FreezerServerOptions opts;
    opts.listen_address = listen;

    apply_tls_env(opts);

    // Optional in-process scheduled backups (PRD §14). Enabled by FMGR_BACKUP_DIR;
    // SQLite-only, so skip an in-memory database (nothing on disk to hot-copy).
    const char* backup_dir_env = std::getenv("FMGR_BACKUP_DIR");
    if (backup_dir_env != nullptr && db_path != ":memory:") {
      const auto env_double = [](const char* name, double fallback) {
        const char* value = std::getenv(name);
        return value != nullptr ? std::strtod(value, nullptr) : fallback;
      };
      const auto env_int = [](const char* name, int fallback) {
        const char* value = std::getenv(name);
        return value != nullptr ? std::atoi(value) : fallback;
      };
      constexpr double k_micros_per_hour = 3'600.0 * 1'000'000.0;
      const char* actor_env = std::getenv("FMGR_BACKUP_ACTOR");
      fmgr::backup::BackupScheduleConfig schedule;
      schedule.sqlite_db_path = db_path;
      schedule.backup_dir = backup_dir_env;
      schedule.retention = fmgr::backup::RetentionPolicy{env_int("FMGR_BACKUP_DAILY", 30),
                                                         env_int("FMGR_BACKUP_MONTHLY", 12),
                                                         env_int("FMGR_BACKUP_YEARLY", 7)};
      schedule.backup_interval_micros = static_cast<std::int64_t>(
          env_double("FMGR_BACKUP_INTERVAL_HOURS", 24.0) * k_micros_per_hour);
      schedule.drill_interval_micros = static_cast<std::int64_t>(
          env_double("FMGR_BACKUP_DRILL_HOURS", 168.0) * k_micros_per_hour);
      schedule.actor =
          actor_env != nullptr ? fmgr::core::UserId::parse(actor_env) : fmgr::core::UserId{};
      opts.backup_schedule = schedule;
    }

    fmgr::server::FreezerServer server(*backend, auth, std::move(opts));
    // build() binds the gRPC port and starts accepting (non-blocking). The REST
    // gateway then dials the in-process channel; drogon::app().run() blocks the
    // main thread for the lifetime of the process.
    server.build();
    fmgr::obs::log_lifecycle(
        fmgr::obs::Level::Info,
        fmt::format("freezerd: gRPC listening on {} (SQLite: {})", listen, db_path),
        "server.grpc_listen");

    fmgr::rest::GatewayStubs stubs(server.in_process_channel());
    fmgr::rest::RestGateway gateway(stubs);
    gateway.register_routes();

    // Unauthenticated /health for a load-balancer readiness probe (PRD §17): a
    // structured per-dependency report, 200 when healthy / 503 when a dependency
    // is down. Bind behind a reverse-proxy ACL or to localhost in production.
    auto health_kms = std::shared_ptr<fmgr::kms::IKmsProvider>(fmgr::kms::make_default_kms());
    const char* health_backup_env = std::getenv("FMGR_BACKUP_DIR");
    const std::string health_backup_dir = health_backup_env != nullptr ? health_backup_env : "";
    fmgr::storage::IStorageBackend* backend_ptr = backend.get();
    fmgr::rest::RestGateway::register_health(fmgr::obs::HealthProbe{
        .database = [backend_ptr] { return probe_database(*backend_ptr); },
        .kms = [health_kms] { return probe_kms(health_kms.get()); },
        .backup = [health_backup_dir] { return probe_backup(health_backup_dir); },
    });

    // Unauthenticated Prometheus scrape endpoint (PRD §17). Bind behind a
    // reverse-proxy ACL or to localhost in production.
    fmgr::rest::RestGateway::register_metrics();

    const char* rest_env = std::getenv("FMGR_REST_LISTEN");
    const std::string rest_listen = rest_env != nullptr ? rest_env : "0.0.0.0:8080";
    const auto [rest_host, rest_port] = parse_listen(rest_listen, 8080);

    const char* rest_cert = std::getenv("FMGR_REST_TLS_CERT");
    const char* rest_key = std::getenv("FMGR_REST_TLS_KEY");
    const bool rest_tls = rest_cert != nullptr && rest_key != nullptr;
    drogon::app().addListener(rest_host, rest_port, rest_tls, rest_cert != nullptr ? rest_cert : "",
                              rest_key != nullptr ? rest_key : "");
    fmgr::obs::log_lifecycle(fmgr::obs::Level::Info,
                             fmt::format("freezerd: REST {} listening on {}:{}",
                                         rest_tls ? "(TLS)" : "(plaintext)", rest_host, rest_port),
                             "server.rest_listen");

    // PRD §6: operators signal a cert rotation with SIGHUP. The gRPC listener uses
    // a FileWatcherCertificateProvider that polls the cert/key/CA files and
    // hot-swaps them for new handshakes without dropping in-flight connections, so
    // a rotation is picked up automatically within FMGR_TLS_RELOAD_SEC. This
    // handler just makes the documented operator gesture observable in the logs;
    // it must stay async-signal-safe, so it only sets a flag (no logging here).
    static volatile std::sig_atomic_t sighup_seen = 0;
    std::signal(SIGHUP, [](int) { sighup_seen = 1; });

    drogon::app().run();
    if (sighup_seen != 0) {
      fmgr::obs::log_lifecycle(fmgr::obs::Level::Info,
                               "SIGHUP received; TLS certificates hot-reload via the file watcher",
                               "tls.sighup");
    }
    server.shutdown();
    return 0;
  } catch (const std::exception& e) {
    fmgr::obs::log_lifecycle(fmgr::obs::Level::Error, fmt::format("freezerd: fatal: {}", e.what()),
                             "server.fatal");
    spdlog::shutdown();
    return 1;
  }
}
