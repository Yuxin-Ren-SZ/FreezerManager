// SPDX-License-Identifier: AGPL-3.0-or-later

#include "server/SampleServiceImpl.h"

#include "cli/SampleCsv.h"
#include "core/custom_field_validator.h"
#include "core/permissions.h"
#include "core/quantity.h"
#include "core/sample.h"
#include "server/GrpcErrorTranslation.h"
#include "storage/CustomFieldResolver.h"
#include "storage/IStorageBackend.h"
#include "storage/SampleOps.h"
#include "storage/SampleTraits.h"

#include <fmgr/v1/sample.grpc.pb.h>
#include <grpcpp/grpcpp.h>
#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <optional>
#include <random>
#include <sstream>
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

    [[nodiscard]] std::string now_iso8601_utc() {
      const auto secs = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
      std::tm tm_buf{};
      gmtime_r(&secs, &tm_buf);
      std::array<char, 32> buf{};
      std::strftime(buf.data(), buf.size(), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
      return {buf.data()};
    }

    // RFC 4122 version-4 UUID from a non-deterministic entropy source. IDs must be
    // unguessable but need not be cryptographically secret (see LabServiceImpl).
    [[nodiscard]] std::string generate_uuid_v4() {
      std::random_device rng;
      std::array<std::uint8_t, 16> bytes{};
      for (std::size_t i = 0; i < bytes.size(); i += 4) {
        const std::uint32_t word = rng();
        bytes[i] = static_cast<std::uint8_t>(word & 0xFFU);
        bytes[i + 1] = static_cast<std::uint8_t>((word >> 8U) & 0xFFU);
        bytes[i + 2] = static_cast<std::uint8_t>((word >> 16U) & 0xFFU);
        bytes[i + 3] = static_cast<std::uint8_t>((word >> 24U) & 0xFFU);
      }
      bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0FU) | 0x40U); // version 4
      bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3FU) | 0x80U); // variant
      std::array<char, 37> buf{};
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
      std::snprintf(buf.data(), buf.size(),
                    "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                    bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
                    bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14],
                    bytes[15]);
      return {buf.data()};
    }

    // ---- SampleStatus / CheckoutAction mapping ----
    //
    // The wire enums mirror the core enums 1:1 (sample.proto was aligned with
    // core::SampleStatus / core::CheckoutAction), so each value round-trips.
    // The *_UNSPECIFIED members have no core counterpart.

    [[nodiscard]] fmgr::v1::SampleStatus to_proto_status(core::SampleStatus status) {
      switch (status) {
      case core::SampleStatus::Active:
        return fmgr::v1::SAMPLE_STATUS_ACTIVE;
      case core::SampleStatus::CheckedOut:
        return fmgr::v1::SAMPLE_STATUS_CHECKED_OUT;
      case core::SampleStatus::Depleted:
        return fmgr::v1::SAMPLE_STATUS_DEPLETED;
      case core::SampleStatus::Destroyed:
        return fmgr::v1::SAMPLE_STATUS_DESTROYED;
      case core::SampleStatus::Tombstoned:
        return fmgr::v1::SAMPLE_STATUS_TOMBSTONED;
      }
      return fmgr::v1::SAMPLE_STATUS_UNSPECIFIED;
    }

    [[nodiscard]] core::SampleStatus from_proto_status(fmgr::v1::SampleStatus status) {
      switch (status) {
      case fmgr::v1::SAMPLE_STATUS_ACTIVE:
        return core::SampleStatus::Active;
      case fmgr::v1::SAMPLE_STATUS_CHECKED_OUT:
        return core::SampleStatus::CheckedOut;
      case fmgr::v1::SAMPLE_STATUS_DEPLETED:
        return core::SampleStatus::Depleted;
      case fmgr::v1::SAMPLE_STATUS_DESTROYED:
        return core::SampleStatus::Destroyed;
      case fmgr::v1::SAMPLE_STATUS_TOMBSTONED:
        return core::SampleStatus::Tombstoned;
      case fmgr::v1::SAMPLE_STATUS_UNSPECIFIED:
      default:
        throw storage::ConstraintViolation("status is required");
      }
    }

    [[nodiscard]] core::CheckoutAction from_proto_action(fmgr::v1::CheckoutAction action) {
      switch (action) {
      case fmgr::v1::CHECKOUT_ACTION_CHECKOUT:
        return core::CheckoutAction::CheckedOut;
      case fmgr::v1::CHECKOUT_ACTION_CHECKIN:
        return core::CheckoutAction::CheckedIn;
      case fmgr::v1::CHECKOUT_ACTION_DISCARD:
        return core::CheckoutAction::Destroyed;
      case fmgr::v1::CHECKOUT_ACTION_UNSPECIFIED:
      default:
        throw storage::ConstraintViolation("action is required");
      }
    }

    // ---- Marshalling: core entity -> protobuf message ----

    void fill_sample(fmgr::v1::Sample* out, const core::Sample& sample) {
      out->set_id(sample.id.to_string());
      out->set_lab_id(sample.lab_id.to_string());
      out->set_item_type_id(sample.item_type_id.to_string());
      out->set_name(sample.name);
      if (sample.barcode.has_value()) {
        out->set_barcode(*sample.barcode);
      }
      if (sample.container_type_id.has_value()) {
        out->set_container_type_id(sample.container_type_id->to_string());
      }
      if (sample.box_id.has_value()) {
        out->set_box_id(sample.box_id->to_string());
      }
      if (sample.position_label.has_value()) {
        out->set_position_label(*sample.position_label);
      }
      if (sample.volume_value.has_value()) {
        out->set_volume_value(static_cast<double>(*sample.volume_value));
      }
      if (sample.volume_unit.has_value()) {
        out->set_volume_unit(std::string(core::to_string(*sample.volume_unit)));
      }
      if (sample.mass_value.has_value()) {
        out->set_mass_value(static_cast<double>(*sample.mass_value));
      }
      if (sample.mass_unit.has_value()) {
        out->set_mass_unit(std::string(core::to_string(*sample.mass_unit)));
      }
      out->set_status(to_proto_status(sample.status));
      if (sample.parent_sample_id.has_value()) {
        out->set_parent_sample_id(sample.parent_sample_id->to_string());
      }
      out->set_created_by(sample.created_by.to_string());
      out->mutable_created_at()->set_unix_micros(sample.created_at.unix_micros());
      out->set_last_modified_by(sample.last_modified_by.to_string());
      out->mutable_last_modified_at()->set_unix_micros(sample.last_modified_at.unix_micros());
      // PHI lives in phi_fields_enc_json (separate column, not exposed here);
      // only the non-PHI custom field blob travels on the wire.
      out->set_custom_fields_json(sample.custom_fields_json);
    }

    // Throws ConstraintViolation listing every custom-field validation error, so
    // the RPC surfaces as INVALID_ARGUMENT. `custom_fields_json` may be empty
    // (treated as an empty object).
    void validate_sample_custom_fields(storage::ITransaction& txn, const core::LabId& lab_id,
                                       const core::ItemTypeId& item_type_id,
                                       const std::string& custom_fields_json) {
      const auto definitions = storage::resolve_custom_field_defs(txn, lab_id, item_type_id);
      const auto fields = custom_fields_json.empty() ? nlohmann::json::object()
                                                     : nlohmann::json::parse(custom_fields_json);
      const auto errors = core::validate_custom_fields(definitions, fields);
      if (!errors.empty()) {
        std::string message = "custom field validation failed:";
        for (const auto& error : errors) {
          message += " [" + error.key + ": " + error.message + "]";
        }
        throw storage::ConstraintViolation(message);
      }
    }

  } // namespace

  SampleServiceImpl::SampleServiceImpl(auth::IAuthProvider& auth, storage::IStorageBackend& backend)
      : auth_(auth), backend_(backend), middleware_(auth) {
    using P = core::Permission;
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.SampleService/ListSamples", P::SampleRead);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.SampleService/GetSample", P::SampleRead);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.SampleService/CreateSample", P::SampleWrite);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.SampleService/UpdateSample", P::SampleWrite);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.SampleService/SoftDeleteSample",
                                      P::SampleDeleteSoft);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.SampleService/MoveSample", P::SampleWrite);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.SampleService/CheckoutSample", P::SampleCheckout);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.SampleService/ExportSamplesCsv", P::SampleRead);
  }

  grpc::Status SampleServiceImpl::ListSamples(grpc::ServerContext* ctx,
                                              const fmgr::v1::ListSamplesRequest* req,
                                              fmgr::v1::ListSamplesResponse* resp) {
    try {
      const auto lab_id = core::LabId::parse(req->lab_id());
      const auto sctx =
          middleware_.authorize(extract_bearer(*ctx), core::Permission::SampleRead, lab_id);

      auto query = storage::Query<core::Sample>::where(
          storage::field<core::Sample, std::string>(core::Sample::Field::LabId) ==
          lab_id.to_string());
      if (req->include_archived()) {
        query = query.include_tombstoned();
      }
      if (req->has_box_id()) {
        query = query.and_where(
            storage::field<core::Sample, std::string>(core::Sample::Field::BoxId) == req->box_id());
      }
      if (req->has_item_type_id()) {
        query = query.and_where(storage::field<core::Sample, std::string>(
                                    core::Sample::Field::ItemTypeId) == req->item_type_id());
      }
      if (req->has_barcode()) {
        query = query.and_where(storage::field<core::Sample, std::string>(
                                    core::Sample::Field::Barcode) == req->barcode());
      }
      if (req->has_status() && req->status() != fmgr::v1::SAMPLE_STATUS_UNSPECIFIED) {
        query = query.and_where(
            storage::field<core::Sample, std::string>(core::Sample::Field::Status) ==
            std::string(core::to_string(from_proto_status(req->status()))));
      }

      // Page token is a plain integer offset; page size 0 means "no limit".
      std::size_t offset = 0;
      if (!req->page().page_token().empty()) {
        offset = static_cast<std::size_t>(std::stoull(req->page().page_token()));
        query = query.offset(offset);
      }
      const auto page_size = req->page().page_size();
      if (page_size > 0) {
        query = query.limit(static_cast<std::size_t>(page_size));
      }

      auto txn = backend_.begin(storage::IsolationLevel::ReadCommitted);
      rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);
      const auto samples = txn->repo<core::Sample>().query(query);
      txn->commit();

      for (const auto& sample : samples) {
        fill_sample(resp->add_samples(), sample);
      }
      // A full page implies there may be more; hand back the next offset.
      if (page_size > 0 && samples.size() == static_cast<std::size_t>(page_size)) {
        resp->mutable_page()->set_next_page_token(std::to_string(offset + samples.size()));
      }
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

  grpc::Status SampleServiceImpl::GetSample(grpc::ServerContext* ctx,
                                            const fmgr::v1::GetSampleRequest* req,
                                            fmgr::v1::GetSampleResponse* resp) {
    try {
      const auto sample_id = core::SampleId::parse(req->sample_id());
      auto sctx = auth_.validate_token(extract_bearer(*ctx));
      if (!sctx.mfa_complete) {
        throw auth::MfaRequired("MFA required before this operation");
      }

      auto txn = backend_.begin(storage::IsolationLevel::ReadCommitted);
      rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);
      const auto sample = txn->repo<core::Sample>().find_by_id(sample_id);
      txn->commit();

      // find_by_id returns tombstoned rows; treat them as not found (soft-delete).
      if (!sample.has_value() || sample->status == core::SampleStatus::Tombstoned) {
        return {grpc::StatusCode::NOT_FOUND, "sample not found"};
      }
      if (!sctx.has_for_lab(sample->lab_id, core::Permission::SampleRead)) {
        throw auth::PermissionDenied("sample.read required for this lab");
      }
      fill_sample(resp->mutable_sample(), *sample);
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

  grpc::Status SampleServiceImpl::CreateSample(grpc::ServerContext* ctx,
                                               const fmgr::v1::CreateSampleRequest* req,
                                               fmgr::v1::CreateSampleResponse* resp) {
    try {
      const auto lab_id = core::LabId::parse(req->lab_id());
      const auto sctx =
          middleware_.authorize(extract_bearer(*ctx), core::Permission::SampleWrite, lab_id);

      const auto item_type_id = core::ItemTypeId::parse(req->item_type_id());
      const auto created_at = now_timestamp();
      core::Sample sample{
          .id = core::SampleId::parse(generate_uuid_v4()),
          .lab_id = lab_id,
          .item_type_id = item_type_id,
          .name = req->name(),
          .barcode = req->has_barcode() ? std::optional<std::string>{req->barcode()} : std::nullopt,
          .container_type_id =
              req->has_container_type_id()
                  ? std::optional<core::ContainerTypeId>{core::ContainerTypeId::parse(
                        req->container_type_id())}
                  : std::nullopt,
          .box_id = req->has_box_id()
                        ? std::optional<core::BoxId>{core::BoxId::parse(req->box_id())}
                        : std::nullopt,
          .position_label = req->has_position_label()
                                ? std::optional<std::string>{req->position_label()}
                                : std::nullopt,
          .volume_value =
              req->has_volume_value()
                  ? std::optional<std::int64_t>{static_cast<std::int64_t>(req->volume_value())}
                  : std::nullopt,
          .volume_unit =
              req->has_volume_unit()
                  ? std::optional<core::VolumeUnit>{core::parse_volume_unit(req->volume_unit())}
                  : std::nullopt,
          .mass_value =
              req->has_mass_value()
                  ? std::optional<std::int64_t>{static_cast<std::int64_t>(req->mass_value())}
                  : std::nullopt,
          .mass_unit = req->has_mass_unit()
                           ? std::optional<core::MassUnit>{core::parse_mass_unit(req->mass_unit())}
                           : std::nullopt,
          .status = core::SampleStatus::Active,
          .parent_sample_id =
              req->has_parent_sample_id()
                  ? std::optional<core::SampleId>{core::SampleId::parse(req->parent_sample_id())}
                  : std::nullopt,
          .created_by = sctx.user_id,
          .created_at = created_at,
          .last_modified_by = sctx.user_id,
          .last_modified_at = created_at,
          .custom_fields_json =
              req->custom_fields_json().empty() ? "{}" : req->custom_fields_json(),
      };

      auto txn = backend_.begin(storage::IsolationLevel::Serializable);
      rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);
      validate_sample_custom_fields(*txn, lab_id, item_type_id, sample.custom_fields_json);
      txn->repo<core::Sample>().insert(sample, make_ctx(sctx, "create_sample"));
      txn->commit();

      fill_sample(resp->mutable_sample(), sample);
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

  grpc::Status SampleServiceImpl::UpdateSample(grpc::ServerContext* ctx,
                                               const fmgr::v1::UpdateSampleRequest* req,
                                               fmgr::v1::UpdateSampleResponse* resp) {
    try {
      const auto& wire = req->sample();
      const auto lab_id = core::LabId::parse(wire.lab_id());
      const auto sample_id = core::SampleId::parse(wire.id());
      const auto sctx =
          middleware_.authorize(extract_bearer(*ctx), core::Permission::SampleWrite, lab_id);

      auto txn = backend_.begin(storage::IsolationLevel::Serializable);
      rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);
      auto existing = txn->repo<core::Sample>().find_by_id(sample_id);
      if (!existing.has_value() || existing->lab_id != lab_id ||
          existing->status == core::SampleStatus::Tombstoned) {
        return {grpc::StatusCode::NOT_FOUND, "sample not found"};
      }

      // Mutable fields only; lab_id, status (lifecycle goes through Checkout /
      // SoftDelete), created_*, and last_modified timestamps are server-owned.
      const auto item_type_id = core::ItemTypeId::parse(wire.item_type_id());
      existing->item_type_id = item_type_id;
      existing->name = wire.name();
      existing->barcode =
          wire.has_barcode() ? std::optional<std::string>{wire.barcode()} : std::nullopt;
      existing->container_type_id =
          wire.has_container_type_id()
              ? std::optional<core::ContainerTypeId>{core::ContainerTypeId::parse(
                    wire.container_type_id())}
              : std::nullopt;
      existing->box_id = wire.has_box_id()
                             ? std::optional<core::BoxId>{core::BoxId::parse(wire.box_id())}
                             : std::nullopt;
      existing->position_label = wire.has_position_label()
                                     ? std::optional<std::string>{wire.position_label()}
                                     : std::nullopt;
      existing->volume_value =
          wire.has_volume_value()
              ? std::optional<std::int64_t>{static_cast<std::int64_t>(wire.volume_value())}
              : std::nullopt;
      existing->volume_unit =
          wire.has_volume_unit()
              ? std::optional<core::VolumeUnit>{core::parse_volume_unit(wire.volume_unit())}
              : std::nullopt;
      existing->mass_value =
          wire.has_mass_value()
              ? std::optional<std::int64_t>{static_cast<std::int64_t>(wire.mass_value())}
              : std::nullopt;
      existing->mass_unit =
          wire.has_mass_unit()
              ? std::optional<core::MassUnit>{core::parse_mass_unit(wire.mass_unit())}
              : std::nullopt;
      existing->parent_sample_id =
          wire.has_parent_sample_id()
              ? std::optional<core::SampleId>{core::SampleId::parse(wire.parent_sample_id())}
              : std::nullopt;
      existing->custom_fields_json =
          wire.custom_fields_json().empty() ? "{}" : wire.custom_fields_json();
      existing->last_modified_by = sctx.user_id;
      existing->last_modified_at = now_timestamp();

      validate_sample_custom_fields(*txn, lab_id, item_type_id, existing->custom_fields_json);
      txn->repo<core::Sample>().update(*existing, make_ctx(sctx, "update_sample"));
      txn->commit();

      fill_sample(resp->mutable_sample(), *existing);
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

  grpc::Status SampleServiceImpl::SoftDeleteSample(grpc::ServerContext* ctx,
                                                   const fmgr::v1::SoftDeleteSampleRequest* req,
                                                   fmgr::v1::SoftDeleteSampleResponse* /*resp*/) {
    try {
      const auto sample_id = core::SampleId::parse(req->sample_id());
      auto sctx = auth_.validate_token(extract_bearer(*ctx));
      if (!sctx.mfa_complete) {
        throw auth::MfaRequired("MFA required before this operation");
      }

      auto txn = backend_.begin(storage::IsolationLevel::Serializable);
      rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);
      const auto existing = txn->repo<core::Sample>().find_by_id(sample_id);
      if (!existing.has_value() || existing->status == core::SampleStatus::Tombstoned) {
        return {grpc::StatusCode::NOT_FOUND, "sample not found"};
      }
      if (!sctx.has_for_lab(existing->lab_id, core::Permission::SampleDeleteSoft)) {
        throw auth::PermissionDenied("sample.delete_soft required for this lab");
      }
      txn->repo<core::Sample>().soft_delete(sample_id, make_ctx(sctx, "soft_delete_sample"));
      txn->commit();
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

  grpc::Status SampleServiceImpl::MoveSample(grpc::ServerContext* ctx,
                                             const fmgr::v1::MoveSampleRequest* req,
                                             fmgr::v1::MoveSampleResponse* resp) {
    try {
      const auto sample_id = core::SampleId::parse(req->sample_id());
      auto sctx = auth_.validate_token(extract_bearer(*ctx));
      if (!sctx.mfa_complete) {
        throw auth::MfaRequired("MFA required before this operation");
      }

      auto txn = backend_.begin(storage::IsolationLevel::Serializable);
      rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);
      const auto existing = txn->repo<core::Sample>().find_by_id(sample_id);
      if (!existing.has_value() || existing->status == core::SampleStatus::Tombstoned) {
        return {grpc::StatusCode::NOT_FOUND, "sample not found"};
      }
      if (!sctx.has_for_lab(existing->lab_id, core::Permission::SampleWrite)) {
        throw auth::PermissionDenied("sample.write required for this lab");
      }

      auto dest_box = req->has_dest_box_id()
                          ? std::optional<core::BoxId>{core::BoxId::parse(req->dest_box_id())}
                          : std::nullopt;
      auto dest_position = req->has_dest_position()
                               ? std::optional<std::string>{req->dest_position()}
                               : std::nullopt;
      const auto moved = storage::move_sample(*txn, sample_id, dest_box, std::move(dest_position),
                                              make_ctx(sctx, "move_sample"));
      txn->commit();

      fill_sample(resp->mutable_sample(), moved);
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

  grpc::Status SampleServiceImpl::CheckoutSample(grpc::ServerContext* ctx,
                                                 const fmgr::v1::CheckoutSampleRequest* req,
                                                 fmgr::v1::CheckoutSampleResponse* resp) {
    try {
      const auto sample_id = core::SampleId::parse(req->sample_id());
      auto sctx = auth_.validate_token(extract_bearer(*ctx));
      if (!sctx.mfa_complete) {
        throw auth::MfaRequired("MFA required before this operation");
      }

      auto txn = backend_.begin(storage::IsolationLevel::Serializable);
      rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);
      const auto existing = txn->repo<core::Sample>().find_by_id(sample_id);
      if (!existing.has_value() || existing->status == core::SampleStatus::Tombstoned) {
        return {grpc::StatusCode::NOT_FOUND, "sample not found"};
      }
      if (!sctx.has_for_lab(existing->lab_id, core::Permission::SampleCheckout)) {
        throw auth::PermissionDenied("sample.checkout required for this lab");
      }

      storage::CheckoutCommand command{
          .action = from_proto_action(req->action()),
          .volume_used = req->has_volume_used() && req->has_volume_unit()
                             ? std::optional<core::Volume>{core::Volume::from_raw(
                                   static_cast<std::int64_t>(req->volume_used()),
                                   core::parse_volume_unit(req->volume_unit()))}
                             : std::nullopt,
          .reason = req->has_reason() ? std::optional<std::string>{req->reason()} : std::nullopt,
          .event_id = core::CheckoutEventId::parse(generate_uuid_v4()),
          .at = now_timestamp(),
      };
      const auto updated =
          storage::apply_checkout(*txn, sample_id, command, make_ctx(sctx, "checkout_sample"));
      txn->commit();

      fill_sample(resp->mutable_sample(), updated);
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

  grpc::Status SampleServiceImpl::ExportSamplesCsv(grpc::ServerContext* ctx,
                                                   const fmgr::v1::ExportSamplesCsvRequest* req,
                                                   fmgr::v1::ExportSamplesCsvResponse* resp) {
    try {
      const auto lab_id = core::LabId::parse(req->lab_id());
      const auto sctx =
          middleware_.authorize(extract_bearer(*ctx), core::Permission::SampleRead, lab_id);

      auto query = storage::Query<core::Sample>::where(
          storage::field<core::Sample, std::string>(core::Sample::Field::LabId) ==
          lab_id.to_string());
      if (req->include_archived()) {
        query = query.include_tombstoned();
      }

      int schema_version = 0;
      std::vector<core::Sample> samples;
      {
        auto txn = backend_.begin(storage::IsolationLevel::ReadCommitted);
        rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);
        samples = txn->repo<core::Sample>().query(query);
        txn->commit();
        schema_version = backend_.current_version().value;
      }

      std::ostringstream out;
      cli::write_sample_csv(out, samples, schema_version, lab_id.to_string(), now_iso8601_utc());
      resp->set_csv_content(out.str());
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

} // namespace fmgr::server
