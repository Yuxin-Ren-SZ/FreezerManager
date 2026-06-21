// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FMGR_SERVER_AUDITSERVICEIMPL_H
#define FMGR_SERVER_AUDITSERVICEIMPL_H

#include "auth/IAuthProvider.h"
#include "rpc/AuthMiddleware.h"
#include "storage/IStorageBackend.h"

#include <fmgr/v1/audit.grpc.pb.h>
#include <grpcpp/grpcpp.h>

namespace fmgr::server {

  // AuditService — read-only access to the append-only hash-chained audit log.
  //
  // All four RPCs are reads: none open a mutating transaction or append an audit
  // row. Gating model:
  //   - Lab-scoped reads (ListAuditEvents / GetAuditEvent / ExportAuditLog with a
  //     lab_id) require AuditRead / AuditExport for that lab.
  //   - Deployment-wide reads (no lab_id) require a deployment SystemAdmin, since
  //     they expose rows across every lab. VerifyAuditChain is always
  //     deployment-wide: the hash chain links every row globally, so a per-lab
  //     subset cannot be verified in isolation.
  class AuditServiceImpl final : public fmgr::v1::AuditService::Service {
  public:
    explicit AuditServiceImpl(auth::IAuthProvider& auth, storage::IStorageBackend& backend);

    grpc::Status ListAuditEvents(grpc::ServerContext* ctx,
                                 const fmgr::v1::ListAuditEventsRequest* req,
                                 fmgr::v1::ListAuditEventsResponse* resp) override;
    grpc::Status GetAuditEvent(grpc::ServerContext* ctx, const fmgr::v1::GetAuditEventRequest* req,
                               fmgr::v1::GetAuditEventResponse* resp) override;
    grpc::Status VerifyAuditChain(grpc::ServerContext* ctx,
                                  const fmgr::v1::VerifyAuditChainRequest* req,
                                  fmgr::v1::VerifyAuditChainResponse* resp) override;
    grpc::Status ExportAuditLog(grpc::ServerContext* ctx,
                                const fmgr::v1::ExportAuditLogRequest* req,
                                fmgr::v1::ExportAuditLogResponse* resp) override;
    grpc::Status WatchAuditFeed(grpc::ServerContext* ctx,
                                const fmgr::v1::WatchAuditFeedRequest* req,
                                grpc::ServerWriter<fmgr::v1::AuditEvent>* writer) override;

  private:
    auth::IAuthProvider& auth_;
    storage::IStorageBackend& backend_;
    rpc::AuthMiddleware middleware_;
  };

} // namespace fmgr::server

#endif // FMGR_SERVER_AUDITSERVICEIMPL_H
