// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FMGR_SERVER_SAMPLESERVICEIMPL_H
#define FMGR_SERVER_SAMPLESERVICEIMPL_H

#include "auth/IAuthProvider.h"
#include "rpc/AuthMiddleware.h"
#include "storage/IStorageBackend.h"

#include <fmgr/v1/sample.grpc.pb.h>
#include <grpcpp/grpcpp.h>

namespace fmgr::server {

  // SampleService — the sample-placement vertical: lifecycle, placement, atomic
  // moves, checkout state machine, and CSV export. Every RPC is lab-scoped.
  //
  // RPCs whose request carries the lab id directly (ListSamples, CreateSample,
  // ExportSamplesCsv) gate up-front via AuthMiddleware::authorize. RPCs that carry
  // only a sample id (GetSample, UpdateSample, SoftDeleteSample, MoveSample,
  // CheckoutSample) resolve the owning lab by loading the row first, then check
  // the per-lab permission against that lab.
  class SampleServiceImpl final : public fmgr::v1::SampleService::Service {
  public:
    explicit SampleServiceImpl(auth::IAuthProvider& auth, storage::IStorageBackend& backend);

    grpc::Status ListSamples(grpc::ServerContext* ctx, const fmgr::v1::ListSamplesRequest* req,
                             fmgr::v1::ListSamplesResponse* resp) override;
    grpc::Status GetSample(grpc::ServerContext* ctx, const fmgr::v1::GetSampleRequest* req,
                           fmgr::v1::GetSampleResponse* resp) override;
    grpc::Status CreateSample(grpc::ServerContext* ctx, const fmgr::v1::CreateSampleRequest* req,
                              fmgr::v1::CreateSampleResponse* resp) override;
    grpc::Status UpdateSample(grpc::ServerContext* ctx, const fmgr::v1::UpdateSampleRequest* req,
                              fmgr::v1::UpdateSampleResponse* resp) override;
    grpc::Status SoftDeleteSample(grpc::ServerContext* ctx,
                                  const fmgr::v1::SoftDeleteSampleRequest* req,
                                  fmgr::v1::SoftDeleteSampleResponse* resp) override;
    grpc::Status MoveSample(grpc::ServerContext* ctx, const fmgr::v1::MoveSampleRequest* req,
                            fmgr::v1::MoveSampleResponse* resp) override;
    grpc::Status CheckoutSample(grpc::ServerContext* ctx,
                                const fmgr::v1::CheckoutSampleRequest* req,
                                fmgr::v1::CheckoutSampleResponse* resp) override;
    grpc::Status ExportSamplesCsv(grpc::ServerContext* ctx,
                                  const fmgr::v1::ExportSamplesCsvRequest* req,
                                  fmgr::v1::ExportSamplesCsvResponse* resp) override;

  private:
    auth::IAuthProvider& auth_;
    storage::IStorageBackend& backend_;
    rpc::AuthMiddleware middleware_;
  };

} // namespace fmgr::server

#endif // FMGR_SERVER_SAMPLESERVICEIMPL_H
