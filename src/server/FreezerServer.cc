// SPDX-License-Identifier: AGPL-3.0-or-later

#include "server/FreezerServer.h"

#include "kms/KmsFactory.h"

#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>
#include <spdlog/spdlog.h>

#include <memory>
#include <stdexcept>
#include <string>

namespace {

  // Build the master-key provider via the shared factory (OS keyring, else env,
  // else none). Returns null (PHI storage disabled) when no key is configured;
  // a malformed-but-present key throws and aborts startup (fail-fast).
  std::unique_ptr<fmgr::kms::IKmsProvider> make_kms() {
    auto kms = fmgr::kms::make_default_kms();
    if (kms == nullptr) {
      spdlog::warn("PHI field encryption disabled: no master KEK configured "
                   "(set CREDENTIALS_DIRECTORY/master_kek or FMGR_MASTER_KEK)");
    }
    return kms;
  }

} // namespace

namespace fmgr::server {

  FreezerServer::FreezerServer(storage::IStorageBackend& backend, auth::IAuthProvider& auth,
                               FreezerServerOptions opts)
      : opts_(std::move(opts)), kms_(make_kms()), auth_svc_(auth, backend),
        session_svc_(auth, backend), lab_svc_(auth, backend), box_svc_(auth, backend),
        item_type_svc_(auth, backend), sample_svc_(auth, backend, kms_.get()),
        role_svc_(auth, backend), audit_svc_(auth, backend), share_svc_(auth, backend) {}

  FreezerServer::~FreezerServer() {
    if (grpc_server_) {
      grpc_server_->Shutdown();
    }
  }

  void FreezerServer::build() {
    // Production guard: never fall back to a plaintext listener when TLS is
    // required. Checked before any port is bound so a misconfiguration aborts
    // startup loudly instead of serving tokens/PHI in the clear.
    if (opts_.require_tls && (opts_.tls_cert_path.empty() || opts_.tls_key_path.empty())) {
      throw std::invalid_argument(
          "TLS is required (require_tls) but tls_cert_path/tls_key_path are not configured");
    }

    grpc::EnableDefaultHealthCheckService(true);

    grpc::ServerBuilder builder;

    if (opts_.tls_cert_path.empty()) {
      builder.AddListeningPort(opts_.listen_address, grpc::InsecureServerCredentials(),
                               &bound_port_);
    } else {
      // TLS certificate loading is deferred to M5 (KMS phase).
      throw std::runtime_error(
          "TLS not yet implemented; run with empty tls_cert_path for dev mode");
    }

    builder.RegisterService(&auth_svc_);
    builder.RegisterService(&session_svc_);
    builder.RegisterService(&lab_svc_);
    builder.RegisterService(&box_svc_);
    builder.RegisterService(&item_type_svc_);
    builder.RegisterService(&sample_svc_);
    builder.RegisterService(&role_svc_);
    builder.RegisterService(&audit_svc_);
    builder.RegisterService(&share_svc_);

    grpc_server_ = builder.BuildAndStart();
    if (!grpc_server_) {
      throw std::runtime_error("failed to start gRPC server on " + opts_.listen_address);
    }
  }

  void FreezerServer::wait() {
    if (grpc_server_) {
      grpc_server_->Wait();
    }
  }

  void FreezerServer::start() {
    build();
    wait();
  }

  void FreezerServer::shutdown() {
    if (grpc_server_) {
      grpc_server_->Shutdown();
    }
  }

  int FreezerServer::bound_port() const {
    return bound_port_;
  }

  std::shared_ptr<grpc::Channel> FreezerServer::in_process_channel() {
    if (!grpc_server_) {
      throw std::runtime_error("in_process_channel() called before build()");
    }
    return grpc_server_->InProcessChannel(grpc::ChannelArguments{});
  }

} // namespace fmgr::server
