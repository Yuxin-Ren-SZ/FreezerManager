// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FMGR_SERVER_FREEZERSERVER_H
#define FMGR_SERVER_FREEZERSERVER_H

#include "auth/IAuthProvider.h"
#include "backup/BackupRunner.h"
#include "kms/IKmsProvider.h"
#include "server/AuditServiceImpl.h"
#include "server/AuthServiceImpl.h"
#include "server/BackupScheduler.h"
#include "server/BoxServiceImpl.h"
#include "server/ItemTypeServiceImpl.h"
#include "server/LabServiceImpl.h"
#include "server/RateLimitInterceptor.h"
#include "server/RoleServiceImpl.h"
#include "server/SampleServiceImpl.h"
#include "server/SessionServiceImpl.h"
#include "server/ShareServiceImpl.h"
#include "storage/IStorageBackend.h"

#include <grpcpp/grpcpp.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>

namespace fmgr::server {

  struct FreezerServerOptions {
    // Listening address, e.g. "0.0.0.0:50051".
    std::string listen_address{"0.0.0.0:50051"};
    // If empty, server starts without TLS (dev mode only). Both must be set
    // together: a cert without a key (or vice versa) is a misconfiguration and
    // build() throws rather than silently falling back to a plaintext listener.
    std::string tls_cert_path;
    std::string tls_key_path;
    // Optional mTLS: PEM bundle of CAs trusted to sign *client* certificates.
    // When set, build() requires and verifies a client certificate on every
    // connection. Left empty, clients are not authenticated at the TLS layer
    // (bearer tokens remain the only caller identity).
    std::string tls_client_ca_path;
    // Production safety guard: when true, build() refuses to start a plaintext
    // server. A misconfiguration that drops TLS (missing cert/key paths) then
    // fails loudly at startup instead of silently exposing bearer tokens and
    // PHI on the wire. Set from FMGR_REQUIRE_TLS in production deployments.
    bool require_tls{false};
    // When set, build() starts an in-process scheduled-backup runner
    // (BackupScheduler) using this config plus a backup KEK from
    // kms::make_backup_kms(). Left empty, no scheduler runs (current behavior).
    std::optional<backup::BackupScheduleConfig> backup_schedule;
    // Hard cap on a single inbound gRPC message (security audit C-10). A frame
    // larger than this is rejected with RESOURCE_EXHAUSTED before the payload is
    // buffered, so a malicious client cannot exhaust server memory. Paired with a
    // grpc::ResourceQuota that bounds the process-wide buffer pool. Default 10 MiB.
    std::size_t max_receive_message_bytes{std::size_t{10} * 1024 * 1024};
    // Hard cap on a single outbound gRPC message. Without it a wide List query
    // can serialize an arbitrarily large response and the peer rejects it late,
    // after the server has already built the whole frame. Default 10 MiB.
    std::size_t max_send_message_bytes{std::size_t{10} * 1024 * 1024};
    // Ceiling on the gRPC buffer pool, in BYTES (grpc::ResourceQuota). This is
    // the process-wide memory bound that pairs with the per-message caps above.
    // Default 512 MiB.
    std::size_t max_grpc_memory_bytes{std::size_t{512} * 1024 * 1024};
    // Ceiling on gRPC-owned threads, a COUNT — deliberately a separate knob from
    // the byte-valued limits above. (Security audit C-13: this was previously
    // derived as max_receive_message_bytes / 4096, i.e. a byte count used as a
    // thread count, which yielded 2560 threads at the default message size and
    // left the memory pool unbounded.) Default 64.
    int max_grpc_threads{64};
    // When true, INTERNAL errors return a generic message to the client and the
    // real detail is only logged server-side (security audit C-11 info leak).
    // Defaults on in release builds, off in debug so developers see detail on the
    // wire. build() applies this to the process-wide error translator.
#ifdef NDEBUG
    bool mask_internal_errors{true};
#else
    bool mask_internal_errors{false};
#endif
    // Two-tier request throttle applied across every service (security audit
    // C-10 DoS). build() installs it for the server's lifetime.
    RateLimitOptions rate_limit;
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
    // The live backend, retained so build() can hand it to the backup scheduler.
    storage::IStorageBackend& backend_;
    // Master-key provider for PHI field encryption. Null when no key is wired
    // (FMGR_MASTER_KEK unset): the deployment then cannot store PHI. Declared
    // before sample_svc_ so it is constructed first (sample_svc_ borrows it).
    std::unique_ptr<kms::IKmsProvider> kms_;
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
    // Process-wide request throttle; installed in build(), uninstalled in the
    // dtor (RAII). Null until build() runs.
    std::unique_ptr<RateLimitInterceptor> rate_limiter_;
    // In-process scheduled-backup runner; null unless opts_.backup_schedule is set
    // and a backup KEK is configured. Stopped/joined in shutdown() and the dtor.
    std::unique_ptr<BackupScheduler> backup_scheduler_;
    int bound_port_{0};
  };

} // namespace fmgr::server

#endif // FMGR_SERVER_FREEZERSERVER_H
