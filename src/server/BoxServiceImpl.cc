// SPDX-License-Identifier: AGPL-3.0-or-later

#include "server/BoxServiceImpl.h"

#include "core/box.h"
#include "core/enums.h"
#include "core/freezer.h"
#include "core/permissions.h"
#include "core/uuid.h"
#include "server/GrpcErrorTranslation.h"
#include "storage/BoxGeometryTraits.h"
#include "storage/FreezerTraits.h"
#include "storage/IStorageBackend.h"

#include <fmgr/v1/box.grpc.pb.h>
#include <grpcpp/grpcpp.h>
#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

namespace fmgr::server {
  namespace {

    [[nodiscard]] storage::MutationContext make_ctx(const auth::SessionContext& sctx,
                                                    std::string_view reason) {
      return storage::MutationContext{
          .actor_user_id = sctx.user_id,
          .actor_session_id = sctx.session_id.to_string(),
          .request_id = "",
          .reason = std::string(reason),
      };
    }

    [[nodiscard]] core::Timestamp now_timestamp() {
      const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();
      return core::Timestamp::from_unix_micros(static_cast<std::int64_t>(micros));
    }

    // Entity IDs are minted from the libsodium CSPRNG via core::generate_uuid_v4
    // so they are unguessable on every platform — std::random_device may degrade
    // to a deterministic engine on some targets (security audit C-1 / review F-2).
    using core::generate_uuid_v4;

    // ---- ContainerKind mapping ----
    //
    // The core enum (Compartment/Shelf/Rack/Drawer/Custom) and the wire enum
    // (UNSPECIFIED/RACK/SHELF/DRAWER/TOWER/GENERIC) do not share an identical
    // set, so the mapping is explicit. Rack/Shelf/Drawer round-trip exactly;
    // Custom <-> GENERIC and Compartment <-> TOWER are the agreed pairings for
    // the two members without a one-to-one wire/core name.

    [[nodiscard]] fmgr::v1::ContainerKind to_proto_kind(core::ContainerKind kind) {
      switch (kind) {
      case core::ContainerKind::Compartment:
        return fmgr::v1::CONTAINER_KIND_TOWER;
      case core::ContainerKind::Shelf:
        return fmgr::v1::CONTAINER_KIND_SHELF;
      case core::ContainerKind::Rack:
        return fmgr::v1::CONTAINER_KIND_RACK;
      case core::ContainerKind::Drawer:
        return fmgr::v1::CONTAINER_KIND_DRAWER;
      case core::ContainerKind::Custom:
        return fmgr::v1::CONTAINER_KIND_GENERIC;
      }
      return fmgr::v1::CONTAINER_KIND_GENERIC;
    }

    [[nodiscard]] core::ContainerKind from_proto_kind(fmgr::v1::ContainerKind kind) {
      switch (kind) {
      case fmgr::v1::CONTAINER_KIND_RACK:
        return core::ContainerKind::Rack;
      case fmgr::v1::CONTAINER_KIND_SHELF:
        return core::ContainerKind::Shelf;
      case fmgr::v1::CONTAINER_KIND_DRAWER:
        return core::ContainerKind::Drawer;
      case fmgr::v1::CONTAINER_KIND_TOWER:
        return core::ContainerKind::Compartment;
      case fmgr::v1::CONTAINER_KIND_GENERIC:
      case fmgr::v1::CONTAINER_KIND_UNSPECIFIED:
      default:
        return core::ContainerKind::Custom;
      }
    }

    // ---- Marshalling: core entity -> protobuf message ----

    void fill_freezer(fmgr::v1::Freezer* out, const core::Freezer& freezer) {
      out->set_id(freezer.id.to_string());
      out->set_lab_id(freezer.lab_id.to_string());
      out->set_name(freezer.name);
      out->set_location(freezer.location);
      out->set_model(freezer.model);
      if (freezer.temp_target_c.has_value()) {
        out->set_temp_target_c(*freezer.temp_target_c);
      }
      out->set_layout_root_id(freezer.layout_root_id.to_string());
      out->mutable_created_at()->set_unix_micros(freezer.created_at.unix_micros());
      if (freezer.archived_at.has_value()) {
        out->mutable_archived_at()->set_unix_micros(freezer.archived_at->unix_micros());
      }
    }

    void fill_storage_container(fmgr::v1::StorageContainer* out,
                                const core::StorageContainer& container) {
      out->set_id(container.id.to_string());
      out->set_lab_id(container.lab_id.to_string());
      if (container.parent_id.has_value()) {
        out->set_parent_id(container.parent_id->to_string());
      }
      out->set_kind(to_proto_kind(container.kind));
      out->set_name(container.name);
      out->set_label(container.label);
      out->set_ordering_index(container.ordering_index);
      if (container.capacity_hint.has_value()) {
        out->set_capacity_hint_json(nlohmann::json(*container.capacity_hint).dump());
      }
      out->mutable_created_at()->set_unix_micros(container.created_at.unix_micros());
      if (container.archived_at.has_value()) {
        out->mutable_archived_at()->set_unix_micros(container.archived_at->unix_micros());
      }
    }

    void fill_container_type(fmgr::v1::ContainerType* out, const core::ContainerType& type) {
      out->set_id(type.id.to_string());
      out->set_lab_id(type.lab_id.to_string());
      out->set_name(type.name);
      out->set_size_class(type.size_class);
      if (type.outer_dimensions_mm.has_value()) {
        out->set_outer_dimensions_json(nlohmann::json(*type.outer_dimensions_mm).dump());
      }
      out->set_material(type.material);
      out->set_supplier_sku(type.supplier_sku);
      out->mutable_created_at()->set_unix_micros(type.created_at.unix_micros());
      if (type.archived_at.has_value()) {
        out->mutable_archived_at()->set_unix_micros(type.archived_at->unix_micros());
      }
    }

    void fill_box_type(fmgr::v1::BoxType* out, const core::BoxType& box_type) {
      out->set_id(box_type.id.to_string());
      out->set_lab_id(box_type.lab_id.to_string());
      out->set_name(box_type.name);
      out->set_manufacturer(box_type.manufacturer);
      out->set_sku(box_type.sku);
      for (const auto& position : box_type.positions) {
        auto* const slot = out->add_positions();
        slot->set_label(position.label);
        slot->set_row(position.row);
        slot->set_col(position.col);
        if (position.z.has_value()) {
          slot->set_z(*position.z);
        }
        for (const auto& accepted : position.accepts) {
          slot->add_accepts(accepted);
        }
      }
      out->mutable_created_at()->set_unix_micros(box_type.created_at.unix_micros());
      if (box_type.archived_at.has_value()) {
        out->mutable_archived_at()->set_unix_micros(box_type.archived_at->unix_micros());
      }
    }

    void fill_box(fmgr::v1::Box* out, const core::Box& box) {
      out->set_id(box.id.to_string());
      out->set_lab_id(box.lab_id.to_string());
      out->set_box_type_id(box.box_type_id.to_string());
      out->set_storage_container_id(box.storage_container_id.to_string());
      out->set_label(box.label);
      if (box.serial.has_value()) {
        out->set_serial(*box.serial);
      }
      if (box.barcode.has_value()) {
        out->set_barcode(*box.barcode);
      }
      out->mutable_created_at()->set_unix_micros(box.created_at.unix_micros());
      if (box.archived_at.has_value()) {
        out->mutable_archived_at()->set_unix_micros(box.archived_at->unix_micros());
      }
    }

    [[nodiscard]] std::vector<core::Position>
    positions_from_proto(const google::protobuf::RepeatedPtrField<fmgr::v1::BoxPosition>& wire) {
      std::vector<core::Position> positions;
      positions.reserve(static_cast<std::size_t>(wire.size()));
      for (const auto& slot : wire) {
        core::Position position{
            .label = slot.label(),
            .row = slot.row(),
            .col = slot.col(),
            .z = slot.has_z() ? std::optional<std::int32_t>{slot.z()} : std::nullopt,
            .accepts = {slot.accepts().begin(), slot.accepts().end()},
        };
        positions.push_back(std::move(position));
      }
      return positions;
    }

  } // namespace

  BoxServiceImpl::BoxServiceImpl(auth::IAuthProvider& auth, storage::IStorageBackend& backend)
      : auth_(auth), backend_(backend), middleware_(auth) {
    using P = core::Permission;
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.BoxService/ListFreezers", P::FreezerConfigure);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.BoxService/GetFreezer", P::FreezerConfigure);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.BoxService/CreateFreezer", P::FreezerConfigure);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.BoxService/UpdateFreezer", P::FreezerConfigure);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.BoxService/ArchiveFreezer", P::FreezerConfigure);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.BoxService/ListStorageContainers",
                                      P::FreezerConfigure);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.BoxService/CreateStorageContainer",
                                      P::FreezerConfigure);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.BoxService/UpdateStorageContainer",
                                      P::FreezerConfigure);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.BoxService/ArchiveStorageContainer",
                                      P::FreezerConfigure);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.BoxService/ListContainerTypes", P::BoxConfigure);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.BoxService/CreateContainerType", P::BoxConfigure);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.BoxService/ListBoxTypes", P::BoxConfigure);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.BoxService/CreateBoxType", P::BoxConfigure);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.BoxService/ListBoxes", P::SampleRead);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.BoxService/GetBox", P::SampleRead);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.BoxService/CreateBox", P::BoxConfigure);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.BoxService/UpdateBox", P::BoxConfigure);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.BoxService/ArchiveBox", P::BoxConfigure);
  }

  // =====================================================================
  // Freezer
  // =====================================================================

  grpc::Status BoxServiceImpl::ListFreezers(grpc::ServerContext* ctx,
                                            const fmgr::v1::ListFreezersRequest* req,
                                            fmgr::v1::ListFreezersResponse* resp) {
    try {
      const auto lab_id = core::LabId::parse(req->lab_id());
      const auto sctx =
          middleware_.authorize(extract_bearer(*ctx), core::Permission::FreezerConfigure, lab_id);

      auto txn = backend_.begin(storage::IsolationLevel::ReadCommitted);
      rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);
      const auto freezers = txn->repo<core::Freezer>().query(storage::Query<core::Freezer>::where(
          storage::field<core::Freezer, std::string>(core::Freezer::Field::LabId) ==
          lab_id.to_string()));
      txn->commit();

      for (const auto& freezer : freezers) {
        fill_freezer(resp->add_freezers(), freezer);
      }
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

  grpc::Status BoxServiceImpl::GetFreezer(grpc::ServerContext* ctx,
                                          const fmgr::v1::GetFreezerRequest* req,
                                          fmgr::v1::GetFreezerResponse* resp) {
    try {
      const auto freezer_id = core::FreezerId::parse(req->freezer_id());
      auto sctx = auth_.validate_token(extract_bearer(*ctx));
      if (!sctx.mfa_complete) {
        throw auth::MfaRequired("MFA required before this operation");
      }

      auto txn = backend_.begin(storage::IsolationLevel::ReadCommitted);
      rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);
      const auto freezer = txn->repo<core::Freezer>().find_by_id(freezer_id);
      txn->commit();

      if (!freezer.has_value() || freezer->archived_at.has_value()) {
        return {grpc::StatusCode::NOT_FOUND, "freezer not found"};
      }
      if (!sctx.has_for_lab(freezer->lab_id, core::Permission::FreezerConfigure)) {
        throw auth::PermissionDenied("freezer.configure required for this lab");
      }
      fill_freezer(resp->mutable_freezer(), *freezer);
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

  grpc::Status BoxServiceImpl::CreateFreezer(grpc::ServerContext* ctx,
                                             const fmgr::v1::CreateFreezerRequest* req,
                                             fmgr::v1::CreateFreezerResponse* resp) {
    try {
      const auto lab_id = core::LabId::parse(req->lab_id());
      const auto sctx =
          middleware_.authorize(extract_bearer(*ctx), core::Permission::FreezerConfigure, lab_id);

      // The interior layout is a tree of StorageContainers rooted at
      // layout_root_id. Per the Freezer invariant the root node is created in the
      // same transaction as the appliance row; the client's layout_root_id is
      // server-assigned here rather than trusted from the request.
      const auto created_at = now_timestamp();
      const core::StorageContainer root{
          .id = core::StorageContainerId::parse(generate_uuid_v4()),
          .lab_id = lab_id,
          .parent_id = std::nullopt,
          .kind = core::ContainerKind::Custom,
          .name = req->name(),
          .label = "",
          .ordering_index = 0,
          .capacity_hint = std::nullopt,
          .created_at = created_at,
      };
      const core::Freezer freezer{
          .id = core::FreezerId::parse(generate_uuid_v4()),
          .lab_id = lab_id,
          .name = req->name(),
          .location = req->location(),
          .model = req->model(),
          .temp_target_c =
              req->has_temp_target_c() ? std::optional<double>{req->temp_target_c()} : std::nullopt,
          .layout_root_id = root.id,
          .created_at = created_at,
      };

      auto txn = backend_.begin(storage::IsolationLevel::Serializable);
      rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);
      const auto mut = make_ctx(sctx, "create_freezer");
      txn->repo<core::StorageContainer>().insert(root, mut);
      txn->repo<core::Freezer>().insert(freezer, mut);
      txn->commit();

      fill_freezer(resp->mutable_freezer(), freezer);
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

  grpc::Status BoxServiceImpl::UpdateFreezer(grpc::ServerContext* ctx,
                                             const fmgr::v1::UpdateFreezerRequest* req,
                                             fmgr::v1::UpdateFreezerResponse* resp) {
    try {
      const auto lab_id = core::LabId::parse(req->freezer().lab_id());
      const auto freezer_id = core::FreezerId::parse(req->freezer().id());
      const auto sctx =
          middleware_.authorize(extract_bearer(*ctx), core::Permission::FreezerConfigure, lab_id);

      auto txn = backend_.begin(storage::IsolationLevel::Serializable);
      rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);
      auto existing = txn->repo<core::Freezer>().find_by_id(freezer_id);
      if (!existing.has_value() || existing->lab_id != lab_id) {
        return {grpc::StatusCode::NOT_FOUND, "freezer not found"};
      }
      // Mutable descriptive fields only; lab_id, layout_root_id and timestamps
      // are not caller-editable.
      existing->name = req->freezer().name();
      existing->location = req->freezer().location();
      existing->model = req->freezer().model();
      existing->temp_target_c = req->freezer().has_temp_target_c()
                                    ? std::optional<double>{req->freezer().temp_target_c()}
                                    : std::nullopt;
      txn->repo<core::Freezer>().update(*existing, make_ctx(sctx, "update_freezer"));
      txn->commit();

      fill_freezer(resp->mutable_freezer(), *existing);
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

  grpc::Status BoxServiceImpl::ArchiveFreezer(grpc::ServerContext* ctx,
                                              const fmgr::v1::ArchiveFreezerRequest* req,
                                              fmgr::v1::ArchiveFreezerResponse* /*resp*/) {
    try {
      const auto freezer_id = core::FreezerId::parse(req->freezer_id());
      auto sctx = auth_.validate_token(extract_bearer(*ctx));
      if (!sctx.mfa_complete) {
        throw auth::MfaRequired("MFA required before this operation");
      }

      auto txn = backend_.begin(storage::IsolationLevel::Serializable);
      rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);
      const auto existing = txn->repo<core::Freezer>().find_by_id(freezer_id);
      if (!existing.has_value()) {
        return {grpc::StatusCode::NOT_FOUND, "freezer not found"};
      }
      if (!sctx.has_for_lab(existing->lab_id, core::Permission::FreezerConfigure)) {
        throw auth::PermissionDenied("freezer.configure required for this lab");
      }
      // The appliance and its layout root are archived together (Freezer invariant).
      const auto mut = make_ctx(sctx, "archive_freezer");
      txn->repo<core::Freezer>().soft_delete(freezer_id, mut);
      txn->repo<core::StorageContainer>().soft_delete(existing->layout_root_id, mut);
      txn->commit();
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

  // =====================================================================
  // StorageContainer
  // =====================================================================

  grpc::Status
  BoxServiceImpl::ListStorageContainers(grpc::ServerContext* ctx,
                                        const fmgr::v1::ListStorageContainersRequest* req,
                                        fmgr::v1::ListStorageContainersResponse* resp) {
    try {
      const auto lab_id = core::LabId::parse(req->lab_id());
      const auto sctx =
          middleware_.authorize(extract_bearer(*ctx), core::Permission::FreezerConfigure, lab_id);

      auto query = storage::Query<core::StorageContainer>::where(
          storage::field<core::StorageContainer, std::string>(
              core::StorageContainer::Field::LabId) == lab_id.to_string());
      if (req->has_parent_id()) {
        query = query.and_where(storage::field<core::StorageContainer, std::string>(
                                    core::StorageContainer::Field::ParentId) == req->parent_id());
      }

      auto txn = backend_.begin(storage::IsolationLevel::ReadCommitted);
      rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);
      const auto containers = txn->repo<core::StorageContainer>().query(query);
      txn->commit();

      for (const auto& container : containers) {
        fill_storage_container(resp->add_containers(), container);
      }
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

  grpc::Status
  BoxServiceImpl::CreateStorageContainer(grpc::ServerContext* ctx,
                                         const fmgr::v1::CreateStorageContainerRequest* req,
                                         fmgr::v1::CreateStorageContainerResponse* resp) {
    try {
      const auto lab_id = core::LabId::parse(req->lab_id());
      const auto sctx =
          middleware_.authorize(extract_bearer(*ctx), core::Permission::FreezerConfigure, lab_id);

      const core::StorageContainer container{
          .id = core::StorageContainerId::parse(generate_uuid_v4()),
          .lab_id = lab_id,
          .parent_id =
              req->has_parent_id()
                  ? std::optional<core::StorageContainerId>{core::StorageContainerId::parse(
                        req->parent_id())}
                  : std::nullopt,
          .kind = from_proto_kind(req->kind()),
          .name = req->name(),
          .label = req->label(),
          .ordering_index = req->ordering_index(),
          .capacity_hint = std::nullopt,
          .created_at = now_timestamp(),
      };

      auto txn = backend_.begin(storage::IsolationLevel::Serializable);
      rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);
      txn->repo<core::StorageContainer>().insert(container, make_ctx(sctx, "create_container"));
      txn->commit();

      fill_storage_container(resp->mutable_container(), container);
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

  grpc::Status
  BoxServiceImpl::UpdateStorageContainer(grpc::ServerContext* ctx,
                                         const fmgr::v1::UpdateStorageContainerRequest* req,
                                         fmgr::v1::UpdateStorageContainerResponse* resp) {
    try {
      const auto lab_id = core::LabId::parse(req->container().lab_id());
      const auto container_id = core::StorageContainerId::parse(req->container().id());
      const auto sctx =
          middleware_.authorize(extract_bearer(*ctx), core::Permission::FreezerConfigure, lab_id);

      auto txn = backend_.begin(storage::IsolationLevel::Serializable);
      rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);
      auto existing = txn->repo<core::StorageContainer>().find_by_id(container_id);
      if (!existing.has_value() || existing->lab_id != lab_id) {
        return {grpc::StatusCode::NOT_FOUND, "storage container not found"};
      }
      existing->parent_id =
          req->container().has_parent_id()
              ? std::optional<core::StorageContainerId>{core::StorageContainerId::parse(
                    req->container().parent_id())}
              : std::nullopt;
      existing->kind = from_proto_kind(req->container().kind());
      existing->name = req->container().name();
      existing->label = req->container().label();
      existing->ordering_index = req->container().ordering_index();
      if (!req->container().capacity_hint_json().empty()) {
        existing->capacity_hint =
            nlohmann::json::parse(req->container().capacity_hint_json()).get<core::CapacityHint>();
      }
      txn->repo<core::StorageContainer>().update(*existing, make_ctx(sctx, "update_container"));
      txn->commit();

      fill_storage_container(resp->mutable_container(), *existing);
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

  grpc::Status
  BoxServiceImpl::ArchiveStorageContainer(grpc::ServerContext* ctx,
                                          const fmgr::v1::ArchiveStorageContainerRequest* req,
                                          fmgr::v1::ArchiveStorageContainerResponse* /*resp*/) {
    try {
      const auto container_id = core::StorageContainerId::parse(req->container_id());
      auto sctx = auth_.validate_token(extract_bearer(*ctx));
      if (!sctx.mfa_complete) {
        throw auth::MfaRequired("MFA required before this operation");
      }

      auto txn = backend_.begin(storage::IsolationLevel::Serializable);
      rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);
      const auto existing = txn->repo<core::StorageContainer>().find_by_id(container_id);
      if (!existing.has_value()) {
        return {grpc::StatusCode::NOT_FOUND, "storage container not found"};
      }
      if (!sctx.has_for_lab(existing->lab_id, core::Permission::FreezerConfigure)) {
        throw auth::PermissionDenied("freezer.configure required for this lab");
      }
      txn->repo<core::StorageContainer>().soft_delete(container_id,
                                                      make_ctx(sctx, "archive_container"));
      txn->commit();
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

  // =====================================================================
  // ContainerType
  // =====================================================================

  grpc::Status BoxServiceImpl::ListContainerTypes(grpc::ServerContext* ctx,
                                                  const fmgr::v1::ListContainerTypesRequest* req,
                                                  fmgr::v1::ListContainerTypesResponse* resp) {
    try {
      const auto lab_id = core::LabId::parse(req->lab_id());
      const auto sctx =
          middleware_.authorize(extract_bearer(*ctx), core::Permission::BoxConfigure, lab_id);

      auto txn = backend_.begin(storage::IsolationLevel::ReadCommitted);
      rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);
      const auto types =
          txn->repo<core::ContainerType>().query(storage::Query<core::ContainerType>::where(
              storage::field<core::ContainerType, std::string>(core::ContainerType::Field::LabId) ==
              lab_id.to_string()));
      txn->commit();

      for (const auto& type : types) {
        fill_container_type(resp->add_container_types(), type);
      }
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

  grpc::Status BoxServiceImpl::CreateContainerType(grpc::ServerContext* ctx,
                                                   const fmgr::v1::CreateContainerTypeRequest* req,
                                                   fmgr::v1::CreateContainerTypeResponse* resp) {
    try {
      const auto lab_id = core::LabId::parse(req->lab_id());
      const auto sctx =
          middleware_.authorize(extract_bearer(*ctx), core::Permission::BoxConfigure, lab_id);

      core::ContainerType type{
          .id = core::ContainerTypeId::parse(generate_uuid_v4()),
          .lab_id = lab_id,
          .name = req->name(),
          .size_class = req->size_class(),
          .outer_dimensions_mm = std::nullopt,
          .material = req->material(),
          .supplier_sku = req->supplier_sku(),
          .created_at = now_timestamp(),
      };
      if (!req->outer_dimensions_json().empty()) {
        type.outer_dimensions_mm =
            nlohmann::json::parse(req->outer_dimensions_json()).get<core::OuterDimensionsMm>();
      }

      auto txn = backend_.begin(storage::IsolationLevel::Serializable);
      rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);
      txn->repo<core::ContainerType>().insert(type, make_ctx(sctx, "create_container_type"));
      txn->commit();

      fill_container_type(resp->mutable_container_type(), type);
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

  // =====================================================================
  // BoxType
  // =====================================================================

  grpc::Status BoxServiceImpl::ListBoxTypes(grpc::ServerContext* ctx,
                                            const fmgr::v1::ListBoxTypesRequest* req,
                                            fmgr::v1::ListBoxTypesResponse* resp) {
    try {
      const auto lab_id = core::LabId::parse(req->lab_id());
      const auto sctx =
          middleware_.authorize(extract_bearer(*ctx), core::Permission::BoxConfigure, lab_id);

      auto txn = backend_.begin(storage::IsolationLevel::ReadCommitted);
      rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);
      const auto box_types = txn->repo<core::BoxType>().query(storage::Query<core::BoxType>::where(
          storage::field<core::BoxType, std::string>(core::BoxType::Field::LabId) ==
          lab_id.to_string()));
      txn->commit();

      for (const auto& box_type : box_types) {
        fill_box_type(resp->add_box_types(), box_type);
      }
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

  grpc::Status BoxServiceImpl::CreateBoxType(grpc::ServerContext* ctx,
                                             const fmgr::v1::CreateBoxTypeRequest* req,
                                             fmgr::v1::CreateBoxTypeResponse* resp) {
    try {
      const auto lab_id = core::LabId::parse(req->lab_id());
      const auto sctx =
          middleware_.authorize(extract_bearer(*ctx), core::Permission::BoxConfigure, lab_id);

      const core::BoxType box_type{
          .id = core::BoxTypeId::parse(generate_uuid_v4()),
          .lab_id = lab_id,
          .name = req->name(),
          .manufacturer = req->manufacturer(),
          .sku = req->sku(),
          .positions = positions_from_proto(req->positions()),
          .created_at = now_timestamp(),
      };

      auto txn = backend_.begin(storage::IsolationLevel::Serializable);
      rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);
      txn->repo<core::BoxType>().insert(box_type, make_ctx(sctx, "create_box_type"));
      txn->commit();

      fill_box_type(resp->mutable_box_type(), box_type);
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

  // =====================================================================
  // Box
  // =====================================================================

  grpc::Status BoxServiceImpl::ListBoxes(grpc::ServerContext* ctx,
                                         const fmgr::v1::ListBoxesRequest* req,
                                         fmgr::v1::ListBoxesResponse* resp) {
    try {
      const auto lab_id = core::LabId::parse(req->lab_id());
      const auto sctx =
          middleware_.authorize(extract_bearer(*ctx), core::Permission::SampleRead, lab_id);

      auto query = storage::Query<core::Box>::where(
          storage::field<core::Box, std::string>(core::Box::Field::LabId) == lab_id.to_string());
      if (req->has_storage_container_id()) {
        query = query.and_where(
            storage::field<core::Box, std::string>(core::Box::Field::StorageContainerId) ==
            req->storage_container_id());
      }

      auto txn = backend_.begin(storage::IsolationLevel::ReadCommitted);
      rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);
      const auto boxes = txn->repo<core::Box>().query(query);
      txn->commit();

      for (const auto& box : boxes) {
        fill_box(resp->add_boxes(), box);
      }
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

  grpc::Status BoxServiceImpl::GetBox(grpc::ServerContext* ctx, const fmgr::v1::GetBoxRequest* req,
                                      fmgr::v1::GetBoxResponse* resp) {
    try {
      const auto box_id = core::BoxId::parse(req->box_id());
      auto sctx = auth_.validate_token(extract_bearer(*ctx));
      if (!sctx.mfa_complete) {
        throw auth::MfaRequired("MFA required before this operation");
      }

      auto txn = backend_.begin(storage::IsolationLevel::ReadCommitted);
      rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);
      const auto box = txn->repo<core::Box>().find_by_id(box_id);
      txn->commit();

      if (!box.has_value() || box->archived_at.has_value()) {
        return {grpc::StatusCode::NOT_FOUND, "box not found"};
      }
      if (!sctx.has_for_lab(box->lab_id, core::Permission::SampleRead)) {
        throw auth::PermissionDenied("sample.read required for this lab");
      }
      fill_box(resp->mutable_box(), *box);
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

  grpc::Status BoxServiceImpl::CreateBox(grpc::ServerContext* ctx,
                                         const fmgr::v1::CreateBoxRequest* req,
                                         fmgr::v1::CreateBoxResponse* resp) {
    try {
      const auto lab_id = core::LabId::parse(req->lab_id());
      const auto sctx =
          middleware_.authorize(extract_bearer(*ctx), core::Permission::BoxConfigure, lab_id);

      const core::Box box{
          .id = core::BoxId::parse(generate_uuid_v4()),
          .lab_id = lab_id,
          .box_type_id = core::BoxTypeId::parse(req->box_type_id()),
          .storage_container_id = core::StorageContainerId::parse(req->storage_container_id()),
          .label = req->label(),
          .serial = req->has_serial() ? std::optional<std::string>{req->serial()} : std::nullopt,
          .barcode = req->has_barcode() ? std::optional<std::string>{req->barcode()} : std::nullopt,
          .created_at = now_timestamp(),
      };

      auto txn = backend_.begin(storage::IsolationLevel::Serializable);
      rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);
      txn->repo<core::Box>().insert(box, make_ctx(sctx, "create_box"));
      txn->commit();

      fill_box(resp->mutable_box(), box);
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

  grpc::Status BoxServiceImpl::UpdateBox(grpc::ServerContext* ctx,
                                         const fmgr::v1::UpdateBoxRequest* req,
                                         fmgr::v1::UpdateBoxResponse* resp) {
    try {
      const auto lab_id = core::LabId::parse(req->box().lab_id());
      const auto box_id = core::BoxId::parse(req->box().id());
      const auto sctx =
          middleware_.authorize(extract_bearer(*ctx), core::Permission::BoxConfigure, lab_id);

      auto txn = backend_.begin(storage::IsolationLevel::Serializable);
      rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);
      auto existing = txn->repo<core::Box>().find_by_id(box_id);
      if (!existing.has_value() || existing->lab_id != lab_id) {
        return {grpc::StatusCode::NOT_FOUND, "box not found"};
      }
      existing->box_type_id = core::BoxTypeId::parse(req->box().box_type_id());
      existing->storage_container_id =
          core::StorageContainerId::parse(req->box().storage_container_id());
      existing->label = req->box().label();
      existing->serial =
          req->box().has_serial() ? std::optional<std::string>{req->box().serial()} : std::nullopt;
      existing->barcode = req->box().has_barcode()
                              ? std::optional<std::string>{req->box().barcode()}
                              : std::nullopt;
      txn->repo<core::Box>().update(*existing, make_ctx(sctx, "update_box"));
      txn->commit();

      fill_box(resp->mutable_box(), *existing);
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

  grpc::Status BoxServiceImpl::ArchiveBox(grpc::ServerContext* ctx,
                                          const fmgr::v1::ArchiveBoxRequest* req,
                                          fmgr::v1::ArchiveBoxResponse* /*resp*/) {
    try {
      const auto box_id = core::BoxId::parse(req->box_id());
      auto sctx = auth_.validate_token(extract_bearer(*ctx));
      if (!sctx.mfa_complete) {
        throw auth::MfaRequired("MFA required before this operation");
      }

      auto txn = backend_.begin(storage::IsolationLevel::Serializable);
      rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);
      const auto existing = txn->repo<core::Box>().find_by_id(box_id);
      if (!existing.has_value()) {
        return {grpc::StatusCode::NOT_FOUND, "box not found"};
      }
      if (!sctx.has_for_lab(existing->lab_id, core::Permission::BoxConfigure)) {
        throw auth::PermissionDenied("box.configure required for this lab");
      }
      txn->repo<core::Box>().soft_delete(box_id, make_ctx(sctx, "archive_box"));
      txn->commit();
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

} // namespace fmgr::server
