// SPDX-License-Identifier: AGPL-3.0-or-later

#include "server/ItemTypeServiceImpl.h"
#include "server/RequestId.h"

#include "core/custom_field_tightening.h"
#include "core/custom_field_validator.h"
#include "core/item_type.h"
#include "core/permissions.h"
#include "core/uuid.h"
#include "server/GrpcErrorTranslation.h"
#include "storage/CustomFieldResolver.h"
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

    // N5: a definition attached to an item type shadows whatever that node
    // inherits for the same key — an ancestor's definition or a lab-global one —
    // and may tighten it but not loosen it. Without this the rule holds only
    // where G3.9's form runs, so `freezerctl`, the Qt client or anything on
    // REST/gRPC could store an override that drops a requirement (#103).
    //
    // Which definition is shadowed is the resolver's ranking
    // (`storage::resolve_inherited_custom_field_defs`, the node's ancestors plus
    // the lab globals, most-derived per key); whether the write is a loosening
    // is the pure `core::tighten_violations`. A lab-global definition has no
    // parent to shadow and is skipped.
    void reject_loosening_inherited_definition(storage::ITransaction& txn,
                                               const core::CustomFieldDefinition& proposed) {
      if (!proposed.item_type_id.has_value()) {
        return;
      }
      const auto inherited = storage::resolve_inherited_custom_field_defs(txn, proposed.lab_id,
                                                                          *proposed.item_type_id);
      for (const auto& parent : inherited) {
        if (parent.key != proposed.key) {
          continue;
        }
        const auto violations = core::tighten_violations(parent, proposed);
        if (!violations.empty()) {
          throw storage::ConstraintViolation(
              "custom field '" + proposed.key +
              "' would loosen the definition it inherits: " + violations.front().message);
        }
      }
    }

    // The other end of the same rule (#115, #121). A definition is identified by
    // *where* it is attached and *which* key it defines, and the update replaces
    // both, so a write that changes either one takes the stored row out of the
    // resolution it was part of: the destination inherits something new — checked
    // by `reject_loosening_inherited_definition` above — and what the row leaves
    // behind is checked here. Checking only the destination leaves the rest
    // silently weaker, and both routes are bypasses no client in this repo
    // exercises: the SPA cannot express a move, and its edit mode never renames a
    // key, so the writers that can are the ones nothing tests.
    //
    // What is left behind is whatever the old (node, key) resolves once the stored
    // row is gone — the same `storage::resolve_inherited_custom_field_defs` the
    // destination check uses, so the two ends cannot disagree about the ranking.
    // Deciding *whether* that is weaker is again the pure
    // `core::tighten_violations`, with "nothing left behind" expressed as
    // `core::removal_violations`.
    //
    // Where a move and a rename differ is what "nothing left behind" means. A move
    // takes the row off its node, so a key with nothing behind it loses the field:
    // that is the removal `removal_violations` measures. A rename keeps the row at
    // its node under a new name, so a key with nothing behind it loses only the
    // author's own spelling — and refusing that would block fixing a typo in a
    // key, which is what a rename is usually for. The rename branch therefore
    // compares against an inherited definition only, and lets a key go when there
    // is none; a row that was *equal* to what it shadowed renames freely too,
    // because no tightening disappears.
    //
    // Both parameters are definitions of one key; which is the stored row and
    // which is the write replacing it is carried by the names, not the type.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    void reject_loosening_abandoned_definition(storage::ITransaction& txn,
                                               const core::CustomFieldDefinition& stored,
                                               const core::CustomFieldDefinition& proposed) {
      const bool moved = stored.item_type_id != proposed.item_type_id;
      const bool renamed = stored.key != proposed.key;
      if (!moved && !renamed) {
        return; // the row keeps its node and its key: it abandons nothing
      }
      if (!stored.item_type_id.has_value()) {
        if (!moved) {
          return; // a lab-global renamed in place: nothing is inherited above it
        }
        // A lab-global is inherited by every item type, so its source subtree is
        // the whole lab: narrowing one onto a single node takes it away from
        // every type outside that node's subtree, and establishing that none of
        // them relied on it would mean sweeping every lineage in the lab on a
        // write path. A global that constrains anything therefore stays global;
        // the way to narrow is to leave it in place and add a tighter definition
        // at the type. A global that constrains nothing may still be narrowed.
        const auto lost = core::removal_violations(stored);
        if (!lost.empty()) {
          throw storage::ConstraintViolation(
              "custom field '" + stored.key +
              "' may not be narrowed from the lab to one item type: " + lost.front().message);
        }
        return;
      }
      const auto inherited =
          storage::resolve_inherited_custom_field_defs(txn, stored.lab_id, *stored.item_type_id);
      std::optional<core::CustomFieldDefinition> left_behind;
      for (const auto& candidate : inherited) {
        if (candidate.key == stored.key) {
          left_behind = candidate;
          break;
        }
      }
      if (!moved && !left_behind.has_value()) {
        return; // a rename with nothing behind the old key: only the name changes
      }
      const auto violations = left_behind.has_value()
                                  ? core::tighten_violations(stored, *left_behind)
                                  : core::removal_violations(stored);
      if (!violations.empty()) {
        throw storage::ConstraintViolation(
            "custom field '" + stored.key +
            (moved ? "' would loosen what the item type it is moved from is left with: "
                   : "' would loosen what its item type is left with after the rename: ") +
            violations.front().message);
      }
    }

  } // namespace

  ItemTypeServiceImpl::ItemTypeServiceImpl(auth::IAuthProvider& auth,
                                           storage::IStorageBackend& backend)
      : auth_(auth), backend_(backend), middleware_(auth) {
    using P = core::Permission;
    // Read/write split for the sample schema (#69): the RPCs a client needs to
    // *render* a generated sample form are sample.read, matching the sample
    // screens that consume them and the SampleRead-gated read paths below. A
    // Member holds sample.read but neither *.define, so gating the catalog read
    // on the define permission left every generated form empty. The mutating
    // RPCs keep *.define -- that is what actually guards the catalog.
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.ItemTypeService/ListItemTypes", P::SampleRead);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.ItemTypeService/GetItemType", P::SampleRead);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.ItemTypeService/CreateItemType", P::ItemTypeDefine);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.ItemTypeService/UpdateItemType", P::ItemTypeDefine);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.ItemTypeService/ArchiveItemType",
                                      P::ItemTypeDefine);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.ItemTypeService/ListCustomFieldDefinitions",
                                      P::SampleRead);
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
      // Reading the catalog is a read: sample.read, not item_type.define (#69).
      const auto sctx =
          middleware_.authorize(extract_bearer(*ctx), core::Permission::SampleRead, lab_id);

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
      // The owning lab is only known after the row is loaded; the read is gated
      // on sample.read, the permission the generated sample form runs under (#69).
      if (!sctx.has_for_lab(item_type->lab_id, core::Permission::SampleRead)) {
        throw auth::PermissionDenied("sample.read required for this lab");
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
      // Reading the custom-field catalog is a read: sample.read, not
      // custom_field.define (#69). Defining fields still needs the define
      // permission (Create/Update/Archive below).
      const auto sctx =
          middleware_.authorize(extract_bearer(*ctx), core::Permission::SampleRead, lab_id);

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
      reject_loosening_inherited_definition(*txn, cfd);

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
      // The row as it stands, before the request replaces it: `item_type_id` is
      // part of the replacement, so a move is decided against where the row is
      // *now* as well as where it is going (#115).
      const auto stored = *existing;
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
      reject_loosening_inherited_definition(*txn, *existing);
      reject_loosening_abandoned_definition(*txn, stored, *existing);
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
