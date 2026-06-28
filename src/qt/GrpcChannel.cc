// SPDX-License-Identifier: AGPL-3.0-or-later

#include "qt/GrpcChannel.h"

#include <utility>

#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>

namespace fmgr::qt {

GrpcChannel::GrpcChannel(std::string target) : target_(std::move(target)) {}

bool GrpcChannel::connect() {
  channel_ =
      grpc::CreateChannel(target_, grpc::InsecureChannelCredentials());
  return channel_ != nullptr;
}

std::unique_ptr<v1::AuthService::Stub> GrpcChannel::makeAuthStub() const {
  if (channel_ == nullptr) {
    return nullptr;
  }
  return v1::AuthService::NewStub(channel_);
}

std::unique_ptr<v1::LabService::Stub> GrpcChannel::makeLabStub() const {
  if (channel_ == nullptr) {
    return nullptr;
  }
  return v1::LabService::NewStub(channel_);
}

std::unique_ptr<v1::BoxService::Stub> GrpcChannel::makeBoxStub() const {
  if (channel_ == nullptr) {
    return nullptr;
  }
  return v1::BoxService::NewStub(channel_);
}

}  // namespace fmgr::qt
