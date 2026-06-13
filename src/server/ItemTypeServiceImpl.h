// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FMGR_SERVER_ITEMTYPESERVICEIMPL_H
#define FMGR_SERVER_ITEMTYPESERVICEIMPL_H

#include "auth/IAuthProvider.h"
#include "rpc/AuthMiddleware.h"
#include "storage/IStorageBackend.h"

#include <fmgr/v1/item_type.grpc.pb.h>
#include <grpcpp/grpcpp.h>

namespace fmgr::server {

  // ItemTypeService — the lab's item-type taxonomy and custom-field catalog.
  // The two resources are gated by distinct lab-scoped permissions: ItemType by
  // ItemTypeDefine, CustomFieldDefinition by CustomFieldDefine (neither is global —
  // see core::is_global_only_permission), so each gate resolves against the
  // request's lab.
  //
  // RPCs whose request carries the lab id directly (List*/Create*/Update*) gate
  // up-front via AuthMiddleware::authorize. RPCs that carry only an entity id
  // (GetItemType, ArchiveItemType, ArchiveCustomFieldDefinition) resolve the
  // owning lab by loading the row first, then check the per-lab permission.
  class ItemTypeServiceImpl final : public fmgr::v1::ItemTypeService::Service {
  public:
    explicit ItemTypeServiceImpl(auth::IAuthProvider& auth, storage::IStorageBackend& backend);

    // ---- ItemType ----
    grpc::Status ListItemTypes(grpc::ServerContext* ctx, const fmgr::v1::ListItemTypesRequest* req,
                               fmgr::v1::ListItemTypesResponse* resp) override;
    grpc::Status GetItemType(grpc::ServerContext* ctx, const fmgr::v1::GetItemTypeRequest* req,
                             fmgr::v1::GetItemTypeResponse* resp) override;
    grpc::Status CreateItemType(grpc::ServerContext* ctx,
                                const fmgr::v1::CreateItemTypeRequest* req,
                                fmgr::v1::CreateItemTypeResponse* resp) override;
    grpc::Status UpdateItemType(grpc::ServerContext* ctx,
                                const fmgr::v1::UpdateItemTypeRequest* req,
                                fmgr::v1::UpdateItemTypeResponse* resp) override;
    grpc::Status ArchiveItemType(grpc::ServerContext* ctx,
                                 const fmgr::v1::ArchiveItemTypeRequest* req,
                                 fmgr::v1::ArchiveItemTypeResponse* resp) override;

    // ---- CustomFieldDefinition ----
    grpc::Status ListCustomFieldDefinitions(grpc::ServerContext* ctx,
                                            const fmgr::v1::ListCfdsRequest* req,
                                            fmgr::v1::ListCfdsResponse* resp) override;
    grpc::Status CreateCustomFieldDefinition(grpc::ServerContext* ctx,
                                             const fmgr::v1::CreateCfdRequest* req,
                                             fmgr::v1::CreateCfdResponse* resp) override;
    grpc::Status UpdateCustomFieldDefinition(grpc::ServerContext* ctx,
                                             const fmgr::v1::UpdateCfdRequest* req,
                                             fmgr::v1::UpdateCfdResponse* resp) override;
    grpc::Status ArchiveCustomFieldDefinition(grpc::ServerContext* ctx,
                                              const fmgr::v1::ArchiveCfdRequest* req,
                                              fmgr::v1::ArchiveCfdResponse* resp) override;

  private:
    auth::IAuthProvider& auth_;
    storage::IStorageBackend& backend_;
    rpc::AuthMiddleware middleware_;
  };

} // namespace fmgr::server

#endif // FMGR_SERVER_ITEMTYPESERVICEIMPL_H
