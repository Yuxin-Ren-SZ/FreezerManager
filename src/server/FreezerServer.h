// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FMGR_SERVER_FREEZERSERVER_H
#define FMGR_SERVER_FREEZERSERVER_H

#include "auth/IAuthProvider.h"
#include "server/AuditServiceImpl.h"
#include "server/AuthServiceImpl.h"
#include "server/BoxServiceImpl.h"
#include "server/ItemTypeServiceImpl.h"
#include "server/LabServiceImpl.h"
#include "server/RoleServiceImpl.h"
#include "server/SampleServiceImpl.h"
#include "server/SessionServiceImpl.h"
#include "server/ShareServiceImpl.h"
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
    // Production safety guard: when true, build() refuses to start a plaintext
    // server. A misconfiguration that drops TLS (missing cert/key paths) then
    // fails loudly at startup instead of silently exposing bearer tokens and
    // PHI on the wire. Set from FMGR_REQUIRE_TLS in production deployments.
    bool require_tls{false};
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

    // In-memory channel to this server's services — no socket, no TLS hop. The
    // REST gateway dials its gRPC stubs over this so a REST request reuses the
    // same handlers (RBAC gate, audit append, transactions) as a native client.
    // Only valid after build() returns.
    [[nodiscard]] std::shared_ptr<grpc::Channel> in_process_channel();

  private:
    FreezerServerOptions opts_;
    AuthServiceImpl auth_svc_;
    SessionServiceImpl session_svc_;
    LabServiceImpl lab_svc_;
    BoxServiceImpl box_svc_;
    ItemTypeServiceImpl item_type_svc_;
    SampleServiceImpl sample_svc_;
    RoleServiceImpl role_svc_;
    AuditServiceImpl audit_svc_;
    ShareServiceImpl share_svc_;
    std::unique_ptr<grpc::Server> grpc_server_;
    int bound_port_{0};
  };

} // namespace fmgr::server

#endif // FMGR_SERVER_FREEZERSERVER_H
