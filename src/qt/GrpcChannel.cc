// SPDX-License-Identifier: AGPL-3.0-or-later

#include "qt/GrpcChannel.h"

#include <fstream>
#include <iterator>
#include <string>
#include <utility>

#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>

namespace fmgr::qt {
namespace {

// Read a PEM bundle whole. Returns false on any failure so the caller can fail
// the connect rather than fall back to an unverified (or insecure) channel.
bool readPem(const std::string& path, std::string* out) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return false;
  }
  out->assign((std::istreambuf_iterator<char>(file)),
              std::istreambuf_iterator<char>());
  return !file.bad() && !out->empty();
}

}  // namespace

GrpcChannel::GrpcChannel(std::string target, TlsOptions tls)
    : target_(std::move(target)), tls_(std::move(tls)) {}

bool GrpcChannel::connect() {
  if (!tls_.enabled) {
    channel_ = grpc::CreateChannel(target_, grpc::InsecureChannelCredentials());
    return channel_ != nullptr;
  }

  grpc::SslCredentialsOptions ssl_opts;
  if (!tls_.rootCaPath.empty() &&
      !readPem(tls_.rootCaPath, &ssl_opts.pem_root_certs)) {
    channel_.reset();
    return false;
  }
  channel_ = grpc::CreateChannel(target_, grpc::SslCredentials(ssl_opts));
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

std::unique_ptr<v1::SampleService::Stub> GrpcChannel::makeSampleStub() const {
  if (channel_ == nullptr) {
    return nullptr;
  }
  return v1::SampleService::NewStub(channel_);
}

}  // namespace fmgr::qt
