// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FMGR_SERVER_FREEZERSERVER_H
#define FMGR_SERVER_FREEZERSERVER_H

#include "auth/IAuthProvider.h"
#include "server/AuthServiceImpl.h"
#include "server/BoxServiceImpl.h"
#include "server/ItemTypeServiceImpl.h"
#include "server/LabServiceImpl.h"
#include "server/SessionServiceImpl.h"
#include "storage/IStorageBackend.h"

#include <grpcpp/grpcpp.h>

#include <memory>
#include <string>

namespace fmgr::server {

  struct FreezerServerOptions {
    // Listening address, e.g. "0.0.0.0:50051".
    std::string listen_address{"0.0.0.0:50051"};
    // If empty, server starts without TLS (dev mode only).
    std::string tls_cert_path;
    std::string tls_key_path;
  };

  // Owns the gRPC server, all service impls, and manages the lifecycle.
  // start() blocks until shutdown() is called from another thread.
  class FreezerServer {
  public:
    explicit FreezerServer(storage::IStorageBackend& backend, auth::IAuthProvider& auth,
                           FreezerServerOptions opts = {});
    ~FreezerServer();

    FreezerServer(const FreezerServer&) = delete;
    FreezerServer& operator=(const FreezerServer&) = delete;

    // Bind the port and start accepting connections. Does NOT block.
    // Must be called before wait() or shutdown().
    void build();

    // Block until shutdown() is called. Must be called after build().
    void wait();

    // Convenience: build() + wait() in one call. Blocks until shutdown().
    void start();

    // Thread-safe; can be called from another thread after build().
    void shutdown();

    // Returns the port actually bound (useful when listen_address uses port 0).
    // Only valid after build() returns.
    [[nodiscard]] int bound_port() const;

  private:
    FreezerServerOptions opts_;
    AuthServiceImpl auth_svc_;
    SessionServiceImpl session_svc_;
    LabServiceImpl lab_svc_;
    BoxServiceImpl box_svc_;
    ItemTypeServiceImpl item_type_svc_;
    std::unique_ptr<grpc::Server> grpc_server_;
    int bound_port_{0};
  };

} // namespace fmgr::server

#endif // FMGR_SERVER_FREEZERSERVER_H
