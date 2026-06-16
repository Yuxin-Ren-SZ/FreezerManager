// SPDX-License-Identifier: AGPL-3.0-or-later

// gRPC client stubs the REST gateway forwards to. They are built over the
// server's in-process channel (in-memory, no socket, no TLS hop), so a REST
// request reuses the exact same service handlers — RBAC gate, audit append,
// and transaction lifecycle — as a native gRPC client.
#ifndef FMGR_REST_GATEWAYSTUBS_H
#define FMGR_REST_GATEWAYSTUBS_H

#include <fmgr/v1/audit.grpc.pb.h>
#include <fmgr/v1/auth.grpc.pb.h>
#include <fmgr/v1/box.grpc.pb.h>
#include <fmgr/v1/item_type.grpc.pb.h>
#include <fmgr/v1/lab.grpc.pb.h>
#include <fmgr/v1/role.grpc.pb.h>
#include <fmgr/v1/sample.grpc.pb.h>
#include <fmgr/v1/session.grpc.pb.h>
#include <fmgr/v1/share.grpc.pb.h>
#include <grpcpp/grpcpp.h>

#include <memory>

namespace fmgr::rest {

  // All nine gRPC services are fronted by the gateway: Auth, Session, Lab,
  // Sample, plus the fan-out set Box, ItemType, Role, Audit, Share.
  struct GatewayStubs {
    explicit GatewayStubs(const std::shared_ptr<grpc::Channel>& channel)
        : auth(fmgr::v1::AuthService::NewStub(channel)),
          session(fmgr::v1::SessionService::NewStub(channel)),
          lab(fmgr::v1::LabService::NewStub(channel)),
          sample(fmgr::v1::SampleService::NewStub(channel)),
          box(fmgr::v1::BoxService::NewStub(channel)),
          item_type(fmgr::v1::ItemTypeService::NewStub(channel)),
          role(fmgr::v1::RoleService::NewStub(channel)),
          audit(fmgr::v1::AuditService::NewStub(channel)),
          share(fmgr::v1::ShareService::NewStub(channel)) {}

    std::unique_ptr<fmgr::v1::AuthService::Stub> auth;
    std::unique_ptr<fmgr::v1::SessionService::Stub> session;
    std::unique_ptr<fmgr::v1::LabService::Stub> lab;
    std::unique_ptr<fmgr::v1::SampleService::Stub> sample;
    std::unique_ptr<fmgr::v1::BoxService::Stub> box;
    std::unique_ptr<fmgr::v1::ItemTypeService::Stub> item_type;
    std::unique_ptr<fmgr::v1::RoleService::Stub> role;
    std::unique_ptr<fmgr::v1::AuditService::Stub> audit;
    std::unique_ptr<fmgr::v1::ShareService::Stub> share;
  };

} // namespace fmgr::rest

#endif // FMGR_REST_GATEWAYSTUBS_H
