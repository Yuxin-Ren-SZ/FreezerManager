// SPDX-License-Identifier: AGPL-3.0-or-later

#include "server/FreezerServer.h"

#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>

#include <memory>
#include <stdexcept>
#include <string>

namespace fmgr::server {

  // Forward declaration (defined in UnimplementedStubs.cc).
  void register_stub_services(grpc::ServerBuilder& builder);

  FreezerServer::FreezerServer(storage::IStorageBackend& backend, auth::IAuthProvider& auth,
                               FreezerServerOptions opts)
      : opts_(std::move(opts)), auth_svc_(auth, backend), session_svc_(auth, backend),
        lab_svc_(auth, backend), box_svc_(auth, backend) {}

  FreezerServer::~FreezerServer() {
    if (grpc_server_) {
      grpc_server_->Shutdown();
    }
  }

  void FreezerServer::build() {
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
    register_stub_services(builder);

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

} // namespace fmgr::server
