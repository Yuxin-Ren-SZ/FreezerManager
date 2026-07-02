// SPDX-License-Identifier: AGPL-3.0-or-later

#include "server/ItemTypeServiceImpl.h"
#include "server/RequestId.h"

#include "core/custom_field_validator.h"
#include "core/item_type.h"
#include "core/permissions.h"
#include "core/uuid.h"
#include "server/GrpcErrorTranslation.h"
#include "storage/IStorageBackend.h"
#include "storage/ItemTypeTraits.h"

#include <fmgr/v1/item_type.grpc.pb.h>
#include <grpcpp/grpcpp.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

namespace fmgr::server {
  namespace {

    [[nodiscard]] storage::MutationContext make_ctx(const grpc::ServerContext& ctx,
                                                    const auth::SessionContext& sctx,
                                                    std::string_view reason) {
      return storage::MutationContext{
          .actor_user_id = sctx.user_id,
          .actor_session_id = sctx.session_id.to_string(),
          .request_id = request_id_from(ctx),
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

    // ---- ScopeKind / FieldDataType mapping ----
    //
    // The wire enums (item_type.proto) are kept 1:1 with the core enums
    // (core::ScopeKind / core::FieldDataType) so no value collapses on the wire.
    // The *_UNSPECIFIED member has no core counterpart; on writes it is rejected
    // as INVALID_ARGUMENT via ConstraintViolation.

    [[nodiscard]] fmgr::v1::ScopeKind to_proto_scope(core::ScopeKind kind) {
      switch (kind) {
      case core::ScopeKind::Sample:
        return fmgr::v1::SCOPE_KIND_SAMPLE;
      case core::ScopeKind::Box:
        return fmgr::v1::SCOPE_KIND_BOX;
      case core::ScopeKind::Freezer:
        return fmgr::v1::SCOPE_KIND_FREEZER;
      case core::ScopeKind::Container:
        return fmgr::v1::SCOPE_KIND_CONTAINER;
      }
      return fmgr::v1::SCOPE_KIND_UNSPECIFIED;
    }

    [[nodiscard]] core::ScopeKind from_proto_scope(fmgr::v1::ScopeKind kind) {
      switch (kind) {
      case fmgr::v1::SCOPE_KIND_SAMPLE:
        return core::ScopeKind::Sample;
      case fmgr::v1::SCOPE_KIND_BOX:
        return core::ScopeKind::Box;
      case fmgr::v1::SCOPE_KIND_FREEZER:
        return core::ScopeKind::Freezer;
      case fmgr::v1::SCOPE_KIND_CONTAINER:
        return core::ScopeKind::Container;
      case fmgr::v1::SCOPE_KIND_UNSPECIFIED:
      default:
        throw storage::ConstraintViolation("scope_kind is required");
      }
    }

    [[nodiscard]] fmgr::v1::FieldDataType to_proto_dtype(core::FieldDataType data_type) {
      switch (data_type) {
      case core::FieldDataType::String:
        return fmgr::v1::FIELD_DATA_TYPE_TEXT;
      case core::FieldDataType::Int:
        return fmgr::v1::FIELD_DATA_TYPE_INT;
      case core::FieldDataType::Float:
        return fmgr::v1::FIELD_DATA_TYPE_FLOAT;
      case core::FieldDataType::Bool:
        return fmgr::v1::FIELD_DATA_TYPE_BOOL;
      case core::FieldDataType::Date:
        return fmgr::v1::FIELD_DATA_TYPE_DATE;
      case core::FieldDataType::Datetime:
        return fmgr::v1::FIELD_DATA_TYPE_DATETIME;
      case core::FieldDataType::Enum:
        return fmgr::v1::FIELD_DATA_TYPE_ENUM;
      case core::FieldDataType::Reference:
        return fmgr::v1::FIELD_DATA_TYPE_REFERENCE;
      }
      return fmgr::v1::FIELD_DATA_TYPE_UNSPECIFIED;
    }

    [[nodiscard]] core::FieldDataType from_proto_dtype(fmgr::v1::FieldDataType data_type) {
      switch (data_type) {
      case fmgr::v1::FIELD_DATA_TYPE_TEXT:
        return core::FieldDataType::String;
      case fmgr::v1::FIELD_DATA_TYPE_INT:
        return core::FieldDataType::Int;
      case fmgr::v1::FIELD_DATA_TYPE_FLOAT:
        return core::FieldDataType::Float;
      case fmgr::v1::FIELD_DATA_TYPE_BOOL:
        return core::FieldDataType::Bool;
      case fmgr::v1::FIELD_DATA_TYPE_DATE:
        return core::FieldDataType::Date;
      case fmgr::v1::FIELD_DATA_TYPE_DATETIME:
        return core::FieldDataType::Datetime;
      case fmgr::v1::FIELD_DATA_TYPE_ENUM:
        return core::FieldDataType::Enum;
      case fmgr::v1::FIELD_DATA_TYPE_REFERENCE:
        return core::FieldDataType::Reference;
      case fmgr::v1::FIELD_DATA_TYPE_UNSPECIFIED:
      default:
        throw storage::ConstraintViolation("data_type is required");
      }
    }

    // ---- Marshalling: core entity -> protobuf message ----

    void fill_item_type(fmgr::v1::ItemType* out, const core::ItemType& item_type) {
      out->set_id(item_type.id.to_string());
      out->set_lab_id(item_type.lab_id.to_string());
      if (item_type.parent_id.has_value()) {
        out->set_parent_id(item_type.parent_id->to_string());
      }
      out->set_name(item_type.name);
      out->mutable_created_at()->set_unix_micros(item_type.created_at.unix_micros());
      if (item_type.archived_at.has_value()) {
        out->mutable_archived_at()->set_unix_micros(item_type.archived_at->unix_micros());
      }
    }

    void fill_cfd(fmgr::v1::CustomFieldDefinition* out, const core::CustomFieldDefinition& cfd) {
      out->set_id(cfd.id.to_string());
      out->set_lab_id(cfd.lab_id.to_string());
      out->set_scope_kind(to_proto_scope(cfd.scope_kind));
      if (cfd.item_type_id.has_value()) {
        out->set_item_type_id(cfd.item_type_id->to_string());
      }
      out->set_key(cfd.key);
      out->set_label(cfd.label);
      out->set_data_type(to_proto_dtype(cfd.data_type));
      out->set_required(cfd.required);
      out->set_validation_json(cfd.validation_json);
      out->set_indexed(cfd.indexed);
      out->set_is_phi(cfd.is_phi);
      out->mutable_created_at()->set_unix_micros(cfd.created_at.unix_micros());
      if (cfd.archived_at.has_value()) {
        out->mutable_archived_at()->set_unix_micros(cfd.archived_at->unix_micros());
      }
    }

    // A PHI field must never be indexed: a JSON-path index would leak plaintext
    // PHI into the index structure (PRD §4.1, L10.3). Reject at definition time.
    void reject_indexed_phi(const core::CustomFieldDefinition& cfd) {
      if (cfd.is_phi && cfd.indexed) {
        throw storage::ConstraintViolation(
            "a PHI custom field may not be indexed (is_phi and indexed are mutually exclusive)");
      }
    }

  } // namespace

  ItemTypeServiceImpl::ItemTypeServiceImpl(auth::IAuthProvider& auth,
                                           storage::IStorageBackend& backend)
      : auth_(auth), backend_(backend), middleware_(auth) {
    using P = core::Permission;
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.ItemTypeService/ListItemTypes", P::ItemTypeDefine);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.ItemTypeService/GetItemType", P::ItemTypeDefine);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.ItemTypeService/CreateItemType", P::ItemTypeDefine);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.ItemTypeService/UpdateItemType", P::ItemTypeDefine);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.ItemTypeService/ArchiveItemType",
                                      P::ItemTypeDefine);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.ItemTypeService/ListCustomFieldDefinitions",
                                      P::CustomFieldDefine);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.ItemTypeService/CreateCustomFieldDefinition",
                                      P::CustomFieldDefine);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.ItemTypeService/UpdateCustomFieldDefinition",
                                      P::CustomFieldDefine);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.ItemTypeService/ArchiveCustomFieldDefinition",
                                      P::CustomFieldDefine);
  }

  // =====================================================================
  // ItemType
  // =====================================================================

  grpc::Status ItemTypeServiceImpl::ListItemTypes(grpc::ServerContext* ctx,
                                                  const fmgr::v1::ListItemTypesRequest* req,
                                                  fmgr::v1::ListItemTypesResponse* resp) {
    try {
      const auto lab_id = core::LabId::parse(req->lab_id());
      const auto sctx =
          middleware_.authorize(extract_bearer(*ctx), core::Permission::ItemTypeDefine, lab_id);

      auto query = storage::Query<core::ItemType>::where(
          storage::field<core::ItemType, std::string>(core::ItemType::Field::LabId) ==
          lab_id.to_string());
      if (req->include_archived()) {
        query = query.include_tombstoned();
      }

      auto txn = backend_.begin(storage::IsolationLevel::ReadCommitted);
      rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);
      const auto item_types = txn->repo<core::ItemType>().query(query);
      txn->commit();

      for (const auto& item_type : item_types) {
        fill_item_type(resp->add_item_types(), item_type);
      }
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

  grpc::Status ItemTypeServiceImpl::GetItemType(grpc::ServerContext* ctx,
                                                const fmgr::v1::GetItemTypeRequest* req,
                                                fmgr::v1::GetItemTypeResponse* resp) {
    try {
      const auto item_type_id = core::ItemTypeId::parse(req->item_type_id());
      auto sctx = auth_.validate_token(extract_bearer(*ctx));
      if (!sctx.mfa_complete) {
        throw auth::MfaRequired("MFA required before this operation");
      }

      auto txn = backend_.begin(storage::IsolationLevel::ReadCommitted);
      rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);
      const auto item_type = txn->repo<core::ItemType>().find_by_id(item_type_id);
      txn->commit();

      if (!item_type.has_value() || item_type->archived_at.has_value()) {
        return {grpc::StatusCode::NOT_FOUND, "item type not found"};
      }
      if (!sctx.has_for_lab(item_type->lab_id, core::Permission::ItemTypeDefine)) {
        throw auth::PermissionDenied("item_type.define required for this lab");
      }
      fill_item_type(resp->mutable_item_type(), *item_type);
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

  grpc::Status ItemTypeServiceImpl::CreateItemType(grpc::ServerContext* ctx,
                                                   const fmgr::v1::CreateItemTypeRequest* req,
                                                   fmgr::v1::CreateItemTypeResponse* resp) {
    try {
      const auto lab_id = core::LabId::parse(req->lab_id());
      const auto sctx =
          middleware_.authorize(extract_bearer(*ctx), core::Permission::ItemTypeDefine, lab_id);

      const core::ItemType item_type{
          .id = core::ItemTypeId::parse(generate_uuid_v4()),
          .lab_id = lab_id,
          .parent_id =
              req->has_parent_id()
                  ? std::optional<core::ItemTypeId>{core::ItemTypeId::parse(req->parent_id())}
                  : std::nullopt,
          .name = req->name(),
          .created_at = now_timestamp(),
      };

      auto txn = backend_.begin(storage::IsolationLevel::Serializable);
      rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);
      txn->repo<core::ItemType>().insert(item_type, make_ctx(*ctx, sctx, "create_item_type"));
      txn->commit();

      fill_item_type(resp->mutable_item_type(), item_type);
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

  grpc::Status ItemTypeServiceImpl::UpdateItemType(grpc::ServerContext* ctx,
                                                   const fmgr::v1::UpdateItemTypeRequest* req,
                                                   fmgr::v1::UpdateItemTypeResponse* resp) {
    try {
      const auto lab_id = core::LabId::parse(req->item_type().lab_id());
      const auto item_type_id = core::ItemTypeId::parse(req->item_type().id());
      const auto sctx =
          middleware_.authorize(extract_bearer(*ctx), core::Permission::ItemTypeDefine, lab_id);

      auto txn = backend_.begin(storage::IsolationLevel::Serializable);
      rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);
      auto existing = txn->repo<core::ItemType>().find_by_id(item_type_id);
      if (!existing.has_value() || existing->lab_id != lab_id) {
        return {grpc::StatusCode::NOT_FOUND, "item type not found"};
      }
      // Mutable fields only; lab_id and timestamps are not caller-editable.
      existing->parent_id = req->item_type().has_parent_id()
                                ? std::optional<core::ItemTypeId>{core::ItemTypeId::parse(
                                      req->item_type().parent_id())}
                                : std::nullopt;
      existing->name = req->item_type().name();
      txn->repo<core::ItemType>().update(*existing, make_ctx(*ctx, sctx, "update_item_type"));
      txn->commit();

      fill_item_type(resp->mutable_item_type(), *existing);
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

  grpc::Status ItemTypeServiceImpl::ArchiveItemType(grpc::ServerContext* ctx,
                                                    const fmgr::v1::ArchiveItemTypeRequest* req,
                                                    fmgr::v1::ArchiveItemTypeResponse* /*resp*/) {
    try {
      const auto item_type_id = core::ItemTypeId::parse(req->item_type_id());
      auto sctx = auth_.validate_token(extract_bearer(*ctx));
      if (!sctx.mfa_complete) {
        throw auth::MfaRequired("MFA required before this operation");
      }

      auto txn = backend_.begin(storage::IsolationLevel::Serializable);
      rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);
      const auto existing = txn->repo<core::ItemType>().find_by_id(item_type_id);
      if (!existing.has_value()) {
        return {grpc::StatusCode::NOT_FOUND, "item type not found"};
      }
      if (!sctx.has_for_lab(existing->lab_id, core::Permission::ItemTypeDefine)) {
        throw auth::PermissionDenied("item_type.define required for this lab");
      }
      txn->repo<core::ItemType>().soft_delete(item_type_id,
                                              make_ctx(*ctx, sctx, "archive_item_type"));
      txn->commit();
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

  // =====================================================================
  // CustomFieldDefinition
  // =====================================================================

  grpc::Status ItemTypeServiceImpl::ListCustomFieldDefinitions(grpc::ServerContext* ctx,
                                                               const fmgr::v1::ListCfdsRequest* req,
                                                               fmgr::v1::ListCfdsResponse* resp) {
    try {
      const auto lab_id = core::LabId::parse(req->lab_id());
      const auto sctx =
          middleware_.authorize(extract_bearer(*ctx), core::Permission::CustomFieldDefine, lab_id);

      auto query = storage::Query<core::CustomFieldDefinition>::where(
          storage::field<core::CustomFieldDefinition, std::string>(
              core::CustomFieldDefinition::Field::LabId) == lab_id.to_string());
      if (req->has_item_type_id()) {
        query = query.and_where(storage::field<core::CustomFieldDefinition, std::string>(
                                    core::CustomFieldDefinition::Field::ItemTypeId) ==
                                req->item_type_id());
      }

      auto txn = backend_.begin(storage::IsolationLevel::ReadCommitted);
      rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);
      const auto cfds = txn->repo<core::CustomFieldDefinition>().query(query);
      txn->commit();

      for (const auto& cfd : cfds) {
        fill_cfd(resp->add_cfds(), cfd);
      }
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

  grpc::Status
  ItemTypeServiceImpl::CreateCustomFieldDefinition(grpc::ServerContext* ctx,
                                                   const fmgr::v1::CreateCfdRequest* req,
                                                   fmgr::v1::CreateCfdResponse* resp) {
    try {
      const auto& wire = req->cfd();
      const auto lab_id = core::LabId::parse(wire.lab_id());
      const auto sctx =
          middleware_.authorize(extract_bearer(*ctx), core::Permission::CustomFieldDefine, lab_id);

      const core::CustomFieldDefinition cfd{
          .id = core::CustomFieldDefinitionId::parse(generate_uuid_v4()),
          .lab_id = lab_id,
          .scope_kind = from_proto_scope(wire.scope_kind()),
          .item_type_id =
              wire.has_item_type_id()
                  ? std::optional<core::ItemTypeId>{core::ItemTypeId::parse(wire.item_type_id())}
                  : std::nullopt,
          .key = wire.key(),
          .label = wire.label(),
          .data_type = from_proto_dtype(wire.data_type()),
          .required = wire.required(),
          .validation_json = wire.validation_json().empty() ? "{}" : wire.validation_json(),
          .indexed = wire.indexed(),
          .is_phi = wire.is_phi(),
          .created_at = now_timestamp(),
      };
      reject_indexed_phi(cfd);

      auto txn = backend_.begin(storage::IsolationLevel::Serializable);
      rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);

      // Cap the number of definitions per entity so a lab admin cannot define
      // thousands of fields and degrade every sample create/update (review F-9).
      auto count_query = storage::Query<core::CustomFieldDefinition>::where(
          storage::field<core::CustomFieldDefinition, std::string>(
              core::CustomFieldDefinition::Field::LabId) == lab_id.to_string());
      if (cfd.item_type_id.has_value()) {
        count_query = count_query.and_where(
            storage::field<core::CustomFieldDefinition, std::string>(
                core::CustomFieldDefinition::Field::ItemTypeId) == cfd.item_type_id->to_string());
      }
      if (txn->repo<core::CustomFieldDefinition>().query(count_query).size() >=
          core::k_max_custom_fields_per_entity) {
        throw storage::ConstraintViolation(
            "custom field limit reached: an entity may have at most " +
            std::to_string(core::k_max_custom_fields_per_entity) + " custom fields");
      }
      txn->repo<core::CustomFieldDefinition>().insert(cfd, make_ctx(*ctx, sctx, "create_cfd"));
      txn->commit();

      fill_cfd(resp->mutable_cfd(), cfd);
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

  grpc::Status
  ItemTypeServiceImpl::UpdateCustomFieldDefinition(grpc::ServerContext* ctx,
                                                   const fmgr::v1::UpdateCfdRequest* req,
                                                   fmgr::v1::UpdateCfdResponse* resp) {
    try {
      const auto& wire = req->cfd();
      const auto lab_id = core::LabId::parse(wire.lab_id());
      const auto cfd_id = core::CustomFieldDefinitionId::parse(wire.id());
      const auto sctx =
          middleware_.authorize(extract_bearer(*ctx), core::Permission::CustomFieldDefine, lab_id);

      auto txn = backend_.begin(storage::IsolationLevel::Serializable);
      rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);
      auto existing = txn->repo<core::CustomFieldDefinition>().find_by_id(cfd_id);
      if (!existing.has_value() || existing->lab_id != lab_id) {
        return {grpc::StatusCode::NOT_FOUND, "custom field definition not found"};
      }
      // Mutable fields only; lab_id and timestamps are not caller-editable.
      existing->scope_kind = from_proto_scope(wire.scope_kind());
      existing->item_type_id =
          wire.has_item_type_id()
              ? std::optional<core::ItemTypeId>{core::ItemTypeId::parse(wire.item_type_id())}
              : std::nullopt;
      existing->key = wire.key();
      existing->label = wire.label();
      existing->data_type = from_proto_dtype(wire.data_type());
      existing->required = wire.required();
      existing->validation_json = wire.validation_json().empty() ? "{}" : wire.validation_json();
      existing->indexed = wire.indexed();
      existing->is_phi = wire.is_phi();
      reject_indexed_phi(*existing);
      txn->repo<core::CustomFieldDefinition>().update(*existing,
                                                      make_ctx(*ctx, sctx, "update_cfd"));
      txn->commit();

      fill_cfd(resp->mutable_cfd(), *existing);
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

  grpc::Status
  ItemTypeServiceImpl::ArchiveCustomFieldDefinition(grpc::ServerContext* ctx,
                                                    const fmgr::v1::ArchiveCfdRequest* req,
                                                    fmgr::v1::ArchiveCfdResponse* /*resp*/) {
    try {
      const auto cfd_id = core::CustomFieldDefinitionId::parse(req->cfd_id());
      auto sctx = auth_.validate_token(extract_bearer(*ctx));
      if (!sctx.mfa_complete) {
        throw auth::MfaRequired("MFA required before this operation");
      }

      auto txn = backend_.begin(storage::IsolationLevel::Serializable);
      rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);
      const auto existing = txn->repo<core::CustomFieldDefinition>().find_by_id(cfd_id);
      if (!existing.has_value()) {
        return {grpc::StatusCode::NOT_FOUND, "custom field definition not found"};
      }
      if (!sctx.has_for_lab(existing->lab_id, core::Permission::CustomFieldDefine)) {
        throw auth::PermissionDenied("custom_field.define required for this lab");
      }
      txn->repo<core::CustomFieldDefinition>().soft_delete(cfd_id,
                                                           make_ctx(*ctx, sctx, "archive_cfd"));
      txn->commit();
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

} // namespace fmgr::server
