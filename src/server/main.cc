// SPDX-License-Identifier: AGPL-3.0-or-later

#include "auth/LocalAuthProvider.h"
#include "server/FreezerServer.h"
#include "storage/sqlite/SqliteBackend.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

// Minimal entry point: reads FMGR_LISTEN (default "0.0.0.0:50051") and
// FMGR_DB_PATH (default ":memory:") from the environment and starts the server
// with a SQLite backend. Full configuration via freezerd.toml is deferred.
int main(int /*argc*/, char* /*argv*/[]) {
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

    std::cerr << "freezerd: listening on " << listen << " (SQLite: " << db_path << ")\n";

    fmgr::server::FreezerServer server(*backend, auth, std::move(opts));
    server.start();
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "freezerd: fatal: " << e.what() << '\n';
    return 1;
  }
}
