// SPDX-License-Identifier: AGPL-3.0-or-later

#include "auth/LocalAuthProvider.h"
#include "server/FreezerServer.h"
#include "storage/sqlite/SqliteBackend.h"

#include <spdlog/async.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <cstddef>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>

// Minimal entry point: reads FMGR_LISTEN (default "0.0.0.0:50051") and
// FMGR_DB_PATH (default ":memory:") from the environment and starts the server
// with a SQLite backend. Full configuration via freezerd.toml is deferred.
int main(int /*argc*/, char* /*argv*/[]) {
  // Async logger with a bounded queue: log writes never block the gRPC thread
  // pool, and an error flood drops the oldest lines instead of building up
  // unbounded memory (audit H-3). Set as the default so spdlog::error() in the
  // gRPC error funnel routes here.
  constexpr std::size_t kLogQueueSize = 8192;
  spdlog::init_thread_pool(kLogQueueSize, 1);
  auto logger = spdlog::create_async_nb<spdlog::sinks::stderr_color_sink_mt>("freezerd");
  spdlog::set_default_logger(logger);
  spdlog::flush_on(spdlog::level::err);

  try {
    const char* listen_env = std::getenv("FMGR_LISTEN");
    const std::string listen = listen_env != nullptr ? listen_env : "0.0.0.0:50051";

    const char* db_env = std::getenv("FMGR_DB_PATH");
    const std::string db_path = db_env != nullptr ? db_env : ":memory:";

    auto backend = std::make_unique<fmgr::storage::SqliteBackend>(
        fmgr::storage::SqliteBackendOptions{.database_path = db_path});
    backend->migrate_to_latest();

    fmgr::auth::LocalAuthProvider auth(*backend);

    fmgr::server::FreezerServerOptions opts;
    opts.listen_address = listen;

    const char* tls_cert_env = std::getenv("FMGR_TLS_CERT");
    if (tls_cert_env != nullptr) {
      opts.tls_cert_path = tls_cert_env;
    }
    const char* tls_key_env = std::getenv("FMGR_TLS_KEY");
    if (tls_key_env != nullptr) {
      opts.tls_key_path = tls_key_env;
    }
    const char* require_tls_env = std::getenv("FMGR_REQUIRE_TLS");
    opts.require_tls = require_tls_env != nullptr && (std::string(require_tls_env) == "1" ||
                                                      std::string(require_tls_env) == "true");

    spdlog::info("freezerd: listening on {} (SQLite: {})", listen, db_path);

    fmgr::server::FreezerServer server(*backend, auth, std::move(opts));
    server.start();
    return 0;
  } catch (const std::exception& e) {
    spdlog::error("freezerd: fatal: {}", e.what());
    spdlog::shutdown();
    return 1;
  }
}
