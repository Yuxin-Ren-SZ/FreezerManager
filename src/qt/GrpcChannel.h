// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_QT_GRPCCHANNEL_H
#define FMGR_QT_GRPCCHANNEL_H

#include <memory>
#include <string>

#include <grpcpp/channel.h>

#include "fmgr/v1/auth.grpc.pb.h"
#include "fmgr/v1/box.grpc.pb.h"
#include "fmgr/v1/lab.grpc.pb.h"

namespace fmgr::qt {

// Owns the gRPC channel to freezerd and mints service stubs over it.
//
// connect() is lazy: gRPC does not open a socket until the first RPC, so it
// returns true even with no server listening. The stub factories return nullptr
// until connect() has run, so callers can treat "no stub" as "not connected".
class GrpcChannel {
 public:
  explicit GrpcChannel(std::string target = "0.0.0.0:50051");

  const std::string& target() const { return target_; }

  // Create (or recreate) the underlying channel. Currently uses insecure
  // credentials; TLS is a later module. Returns true once a channel exists.
  bool connect();

  bool isConnected() const { return channel_ != nullptr; }
  std::shared_ptr<grpc::Channel> channel() const { return channel_; }

  // Returns nullptr if not connected.
  std::unique_ptr<v1::AuthService::Stub> makeAuthStub() const;
  std::unique_ptr<v1::LabService::Stub> makeLabStub() const;
  std::unique_ptr<v1::BoxService::Stub> makeBoxStub() const;

 private:
  std::string target_;
  std::shared_ptr<grpc::Channel> channel_;
};

}  // namespace fmgr::qt

#endif  // FMGR_QT_GRPCCHANNEL_H
