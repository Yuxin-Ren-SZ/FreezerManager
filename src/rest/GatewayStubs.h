// SPDX-License-Identifier: AGPL-3.0-or-later

// gRPC client stubs the REST gateway forwards to. They are built over the
// server's in-process channel (in-memory, no socket, no TLS hop), so a REST
// request reuses the exact same service handlers — RBAC gate, audit append,
// and transaction lifecycle — as a native gRPC client.
#ifndef FMGR_REST_GATEWAYSTUBS_H
#define FMGR_REST_GATEWAYSTUBS_H

#include <fmgr/v1/auth.grpc.pb.h>
#include <fmgr/v1/lab.grpc.pb.h>
#include <fmgr/v1/sample.grpc.pb.h>
#include <fmgr/v1/session.grpc.pb.h>
#include <grpcpp/grpcpp.h>

#include <memory>

namespace fmgr::rest {

  // First gateway slice: Auth + Session + Lab + Sample. The remaining services
  // (Box, ItemType, Role, Audit, Share) are added here on fan-out.
  struct GatewayStubs {
    explicit GatewayStubs(const std::shared_ptr<grpc::Channel>& channel)
        : auth(fmgr::v1::AuthService::NewStub(channel)),
          session(fmgr::v1::SessionService::NewStub(channel)),
          lab(fmgr::v1::LabService::NewStub(channel)),
          sample(fmgr::v1::SampleService::NewStub(channel)) {}

    std::unique_ptr<fmgr::v1::AuthService::Stub> auth;
    std::unique_ptr<fmgr::v1::SessionService::Stub> session;
    std::unique_ptr<fmgr::v1::LabService::Stub> lab;
    std::unique_ptr<fmgr::v1::SampleService::Stub> sample;
  };

} // namespace fmgr::rest

#endif // FMGR_REST_GATEWAYSTUBS_H
