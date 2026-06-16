// SPDX-License-Identifier: AGPL-3.0-or-later

#include "auth/LocalAuthProvider.h"
#include "rest/GatewayStubs.h"
#include "rest/RestGateway.h"
#include "server/FreezerServer.h"
#include "storage/sqlite/SqliteBackend.h"

#include <drogon/drogon.h>
#include <spdlog/async.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
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

} // namespace

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

    fmgr::server::FreezerServer server(*backend, auth, std::move(opts));
    // build() binds the gRPC port and starts accepting (non-blocking). The REST
    // gateway then dials the in-process channel; drogon::app().run() blocks the
    // main thread for the lifetime of the process.
    server.build();
    spdlog::info("freezerd: gRPC listening on {} (SQLite: {})", listen, db_path);

    fmgr::rest::GatewayStubs stubs(server.in_process_channel());
    fmgr::rest::RestGateway gateway(stubs);
    gateway.register_routes();

    const char* rest_env = std::getenv("FMGR_REST_LISTEN");
    const std::string rest_listen = rest_env != nullptr ? rest_env : "0.0.0.0:8080";
    const auto [rest_host, rest_port] = parse_listen(rest_listen, 8080);

    const char* rest_cert = std::getenv("FMGR_REST_TLS_CERT");
    const char* rest_key = std::getenv("FMGR_REST_TLS_KEY");
    const bool rest_tls = rest_cert != nullptr && rest_key != nullptr;
    drogon::app().addListener(rest_host, rest_port, rest_tls, rest_cert != nullptr ? rest_cert : "",
                              rest_key != nullptr ? rest_key : "");
    spdlog::info("freezerd: REST {} listening on {}:{}", rest_tls ? "(TLS)" : "(plaintext)",
                 rest_host, rest_port);

    drogon::app().run();
    server.shutdown();
    return 0;
  } catch (const std::exception& e) {
    spdlog::error("freezerd: fatal: {}", e.what());
    spdlog::shutdown();
    return 1;
  }
}
