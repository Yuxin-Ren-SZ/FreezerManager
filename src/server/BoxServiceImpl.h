// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FMGR_SERVER_BOXSERVICEIMPL_H
#define FMGR_SERVER_BOXSERVICEIMPL_H

#include "auth/IAuthProvider.h"
#include "rpc/AuthMiddleware.h"
#include "storage/IStorageBackend.h"

#include <fmgr/v1/box.grpc.pb.h>
#include <grpcpp/grpcpp.h>

namespace fmgr::server {

  // BoxService — the freezer/storage-layout and box-geometry vertical. Every RPC
  // is lab-scoped: the lab already exists, so each gate resolves against the
  // request's lab (BoxConfigure / FreezerConfigure are never global — see
  // core::is_global_only_permission). Read RPCs on physical boxes (ListBoxes,
  // GetBox) are gated by SampleRead because a box is a sample-visibility surface,
  // matching the registry the stub established.
  //
  // RPCs whose request carries the lab id directly (List*/Create*/Update*) gate
  // up-front via AuthMiddleware::authorize. RPCs that carry only an entity id
  // (GetFreezer, ArchiveFreezer, ArchiveStorageContainer, GetBox, ArchiveBox)
  // resolve the owning lab by loading the row first, then check the per-lab
  // permission against that lab.
  class BoxServiceImpl final : public fmgr::v1::BoxService::Service {
  public:
    explicit BoxServiceImpl(auth::IAuthProvider& auth, storage::IStorageBackend& backend);

    // ---- Freezer ----
    grpc::Status ListFreezers(grpc::ServerContext* ctx, const fmgr::v1::ListFreezersRequest* req,
                              fmgr::v1::ListFreezersResponse* resp) override;
    grpc::Status GetFreezer(grpc::ServerContext* ctx, const fmgr::v1::GetFreezerRequest* req,
                            fmgr::v1::GetFreezerResponse* resp) override;
    grpc::Status CreateFreezer(grpc::ServerContext* ctx, const fmgr::v1::CreateFreezerRequest* req,
                               fmgr::v1::CreateFreezerResponse* resp) override;
    grpc::Status UpdateFreezer(grpc::ServerContext* ctx, const fmgr::v1::UpdateFreezerRequest* req,
                               fmgr::v1::UpdateFreezerResponse* resp) override;
    grpc::Status ArchiveFreezer(grpc::ServerContext* ctx,
                                const fmgr::v1::ArchiveFreezerRequest* req,
                                fmgr::v1::ArchiveFreezerResponse* resp) override;

    // ---- StorageContainer ----
    grpc::Status ListStorageContainers(grpc::ServerContext* ctx,
                                       const fmgr::v1::ListStorageContainersRequest* req,
                                       fmgr::v1::ListStorageContainersResponse* resp) override;
    grpc::Status CreateStorageContainer(grpc::ServerContext* ctx,
                                        const fmgr::v1::CreateStorageContainerRequest* req,
                                        fmgr::v1::CreateStorageContainerResponse* resp) override;
    grpc::Status UpdateStorageContainer(grpc::ServerContext* ctx,
                                        const fmgr::v1::UpdateStorageContainerRequest* req,
                                        fmgr::v1::UpdateStorageContainerResponse* resp) override;
    grpc::Status ArchiveStorageContainer(grpc::ServerContext* ctx,
                                         const fmgr::v1::ArchiveStorageContainerRequest* req,
                                         fmgr::v1::ArchiveStorageContainerResponse* resp) override;

    // ---- ContainerType ----
    grpc::Status ListContainerTypes(grpc::ServerContext* ctx,
                                    const fmgr::v1::ListContainerTypesRequest* req,
                                    fmgr::v1::ListContainerTypesResponse* resp) override;
    grpc::Status CreateContainerType(grpc::ServerContext* ctx,
                                     const fmgr::v1::CreateContainerTypeRequest* req,
                                     fmgr::v1::CreateContainerTypeResponse* resp) override;

    // ---- BoxType ----
    grpc::Status ListBoxTypes(grpc::ServerContext* ctx, const fmgr::v1::ListBoxTypesRequest* req,
                              fmgr::v1::ListBoxTypesResponse* resp) override;
    grpc::Status CreateBoxType(grpc::ServerContext* ctx, const fmgr::v1::CreateBoxTypeRequest* req,
                               fmgr::v1::CreateBoxTypeResponse* resp) override;

    // ---- Box ----
    grpc::Status ListBoxes(grpc::ServerContext* ctx, const fmgr::v1::ListBoxesRequest* req,
                           fmgr::v1::ListBoxesResponse* resp) override;
    grpc::Status GetBox(grpc::ServerContext* ctx, const fmgr::v1::GetBoxRequest* req,
                        fmgr::v1::GetBoxResponse* resp) override;
    grpc::Status CreateBox(grpc::ServerContext* ctx, const fmgr::v1::CreateBoxRequest* req,
                           fmgr::v1::CreateBoxResponse* resp) override;
    grpc::Status UpdateBox(grpc::ServerContext* ctx, const fmgr::v1::UpdateBoxRequest* req,
                           fmgr::v1::UpdateBoxResponse* resp) override;
    grpc::Status ArchiveBox(grpc::ServerContext* ctx, const fmgr::v1::ArchiveBoxRequest* req,
                            fmgr::v1::ArchiveBoxResponse* resp) override;

  private:
    auth::IAuthProvider& auth_;
    storage::IStorageBackend& backend_;
    rpc::AuthMiddleware middleware_;
  };

} // namespace fmgr::server

#endif // FMGR_SERVER_BOXSERVICEIMPL_H
