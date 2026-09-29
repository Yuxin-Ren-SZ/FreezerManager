// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_QT_GRPCCHANNEL_H
#define FMGR_QT_GRPCCHANNEL_H

#include <memory>
#include <string>

#include <grpcpp/channel.h>

#include "fmgr/v1/auth.grpc.pb.h"
#include "fmgr/v1/box.grpc.pb.h"
#include "fmgr/v1/lab.grpc.pb.h"
#include "fmgr/v1/sample.grpc.pb.h"

namespace fmgr::qt {

// Transport security for the channel. Off by default so a local dev server
// (plaintext) still works out of the box; production deployments turn it on via
// ConfigManager. There is no automatic fallback from TLS to plaintext — a
// misconfigured root CA fails the connect instead of silently downgrading.
struct TlsOptions {
  bool enabled{false};
  // PEM bundle used to verify the server certificate. Empty = use the system
  // trust store; set this for a self-signed lab server.
  std::string rootCaPath;
};

// Owns the gRPC channel to freezerd and mints service stubs over it.
//
// connect() is lazy: gRPC does not open a socket until the first RPC, so it
// returns true even with no server listening. The stub factories return nullptr
// until connect() has run, so callers can treat "no stub" as "not connected".
class GrpcChannel {
 public:
  explicit GrpcChannel(std::string target = "0.0.0.0:50051",
                       TlsOptions tls = {});

  const std::string& target() const { return target_; }
  const TlsOptions& tls() const { return tls_; }

  // Create (or recreate) the underlying channel. Returns false (and leaves the
  // channel unset) when TLS is enabled but the configured root CA cannot be
  // read — never falls back to an insecure channel.
  bool connect();

  bool isConnected() const { return channel_ != nullptr; }
  std::shared_ptr<grpc::Channel> channel() const { return channel_; }

  // Returns nullptr if not connected.
  std::unique_ptr<v1::AuthService::Stub> makeAuthStub() const;
  std::unique_ptr<v1::LabService::Stub> makeLabStub() const;
  std::unique_ptr<v1::BoxService::Stub> makeBoxStub() const;
  std::unique_ptr<v1::SampleService::Stub> makeSampleStub() const;

 private:
  std::string target_;
  TlsOptions tls_;
  std::shared_ptr<grpc::Channel> channel_;
};

}  // namespace fmgr::qt

#endif  // FMGR_QT_GRPCCHANNEL_H
