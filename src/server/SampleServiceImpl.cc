// SPDX-License-Identifier: AGPL-3.0-or-later

#include "server/SampleServiceImpl.h"
#include "server/RequestId.h"

#include "cli/CsvReader.h"
#include "cli/SampleCsv.h"
#include "cli/SampleImport.h"
#include "core/custom_field_validator.h"
#include "core/identity.h"
#include "core/permissions.h"
#include "core/quantity.h"
#include "core/sample.h"
#include "core/uuid.h"
#include "crypto/FieldCipher.h"
#include "server/GrpcErrorTranslation.h"
#include "storage/CustomFieldResolver.h"
#include "storage/IStorageBackend.h"
#include "storage/IdentityTraits.h"
#include "storage/SampleOps.h"
#include "storage/SampleTraits.h"

#include <fmgr/v1/sample.grpc.pb.h>
#include <grpcpp/grpcpp.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

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

    [[nodiscard]] std::int64_t now_unix_micros() {
      return now_timestamp().unix_micros();
    }

    // WatchSampleList poll cadence: tail the table once per interval but wake
    // every slice so cancellation is observed promptly. Mirrors WatchAuditFeed.
    constexpr std::chrono::milliseconds k_watch_poll_slice{100};
    constexpr int k_watch_poll_slices = 10; // 100ms * 10 = ~1s between polls

    [[nodiscard]] std::string now_iso8601_utc() {
      const auto secs = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
      std::tm tm_buf{};
      gmtime_r(&secs, &tm_buf);
      std::array<char, 32> buf{};
      std::strftime(buf.data(), buf.size(), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
      return {buf.data()};
    }

    // Entity IDs are minted from the libsodium CSPRNG via core::generate_uuid_v4
    // so they are unguessable on every platform — std::random_device may degrade
    // to a deterministic engine on some targets (security audit C-1 / review F-2).
    using core::generate_uuid_v4;

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

    // Outcome of partitioning an incoming custom-field blob into the plaintext
    // (non-PHI) column and the encrypted PHI envelope.
    struct PreparedCustomFields {
      std::string custom_fields_json{"{}"};  // non-PHI, validated
      std::string phi_fields_enc_json{"{}"}; // AEAD envelope; "{}" when no PHI
    };

    // Validate the combined incoming custom fields, then split them: PHI-tagged
    // keys are encrypted into the envelope under a fresh per-record DEK, the rest
    // stay in the plaintext column. Throws ConstraintViolation (→ INVALID_ARGUMENT)
    // on validation failure or when a PHI value is supplied for a lab that has PHI
    // mode disabled. Throws when PHI is supplied but no KMS is configured.
    PreparedCustomFields prepare_custom_fields(storage::ITransaction& txn,
                                               const core::LabId& lab_id,
                                               const core::ItemTypeId& item_type_id,
                                               const std::string& incoming_json,
                                               const kms::IKmsProvider* kms) {
      const auto definitions = storage::resolve_custom_field_defs(txn, lab_id, item_type_id);
      const auto incoming =
          incoming_json.empty() ? nlohmann::json::object() : nlohmann::json::parse(incoming_json);
      const auto errors = core::validate_custom_fields(definitions, incoming);
      if (!errors.empty()) {
        std::string message = "custom field validation failed:";
        for (const auto& error : errors) {
          message += " [" + error.key + ": " + error.message + "]";
        }
        throw storage::ConstraintViolation(message);
      }

      std::set<std::string> phi_keys;
      for (const auto& def : definitions) {
        if (def.is_phi) {
          phi_keys.insert(def.key);
        }
      }

      PreparedCustomFields prepared;
      nlohmann::json non_phi = nlohmann::json::object();
      crypto::PhiFields phi;
      if (incoming.is_object()) {
        for (const auto& [key, value] : incoming.items()) {
          if (phi_keys.contains(key)) {
            phi.emplace(key, value);
          } else {
            non_phi[key] = value;
          }
        }
      }
      prepared.custom_fields_json = non_phi.dump();

      if (!phi.empty()) {
        const auto lab = txn.repo<core::Lab>().find_by_id(lab_id);
        if (!lab.has_value() || !lab->is_phi_enabled) {
          throw storage::ConstraintViolation(
              "PHI custom fields supplied but PHI mode is disabled for this lab");
        }
        if (kms == nullptr) {
          // Server misconfiguration, not a client error: no master key is wired.
          throw std::runtime_error(
              "server is not configured with a master key; cannot store PHI fields");
        }
        prepared.phi_fields_enc_json = crypto::encrypt(phi, *kms);
      }
      return prepared;
    }

    // If the caller holds phi.read for the sample's lab and the sample carries a
    // PHI envelope, decrypt it, merge the PHI fields into the response's
    // custom_fields_json, and return the disclosed key names (for the PHI-read
    // audit event). Returns empty when there is no PHI, no permission, or no KMS.
    std::vector<std::string> reveal_phi(fmgr::v1::Sample* out, const core::Sample& sample,
                                        const auth::SessionContext& sctx,
                                        const kms::IKmsProvider* kms) {
      if (sample.phi_fields_enc_json.empty() || sample.phi_fields_enc_json == "{}") {
        return {};
      }
      if (kms == nullptr || !sctx.has_for_lab(sample.lab_id, core::Permission::PhiRead)) {
        return {}; // PHI stays hidden; no decryption attempted.
      }
      const crypto::PhiFields phi = crypto::decrypt(sample.phi_fields_enc_json, *kms);
      if (phi.empty()) {
        return {};
      }
      auto merged = sample.custom_fields_json.empty()
                        ? nlohmann::json::object()
                        : nlohmann::json::parse(sample.custom_fields_json);
      std::vector<std::string> disclosed;
      disclosed.reserve(phi.size());
      for (const auto& [key, value] : phi) {
        merged[key] = value;
        disclosed.push_back(key);
      }
      out->set_custom_fields_json(merged.dump());
      return disclosed;
    }

    // Tail query for WatchSampleList: lab scope + the request's box/item-type
    // filters plus a `last_modified_at >= cursor` lower bound, ordered
    // (last_modified_at, id) so the cursor advances monotonically. Tombstoned
    // rows are intentionally included (include_tombstoned) so a soft-delete is
    // delivered as a SAMPLE_STATUS_TOMBSTONED row the client can remove.
    [[nodiscard]] storage::Query<core::Sample>
    build_sample_watch_query(const fmgr::v1::WatchSampleListRequest& req, const core::LabId& lab_id,
                             std::int64_t cursor_micros) {
      auto query =
          storage::Query<core::Sample>::where(storage::field<core::Sample, std::string>(
                                                  core::Sample::Field::LabId) == lab_id.to_string())
              .include_tombstoned();
      if (req.has_box_id()) {
        query = query.and_where(
            storage::field<core::Sample, std::string>(core::Sample::Field::BoxId) == req.box_id());
      }
      if (req.has_item_type_id()) {
        query = query.and_where(storage::field<core::Sample, std::string>(
                                    core::Sample::Field::ItemTypeId) == req.item_type_id());
      }
      return query
          .and_where(storage::greater_or_equal(
              storage::field<core::Sample, core::Timestamp>(core::Sample::Field::LastModifiedAt),
              core::Timestamp::from_unix_micros(cursor_micros)))
          .order_by(
              storage::field<core::Sample, core::Timestamp>(core::Sample::Field::LastModifiedAt),
              storage::SortDirection::Ascending)
          .order_by(storage::field<core::Sample, std::string>(core::Sample::Field::Id),
                    storage::SortDirection::Ascending);
    }

    // Write rows newer than the cursor, suppressing ids already delivered at the
    // exact cursor microsecond (the lower bound is `>=`, so a same-micro row can
    // reappear). Advances the cursor and rebuilds the dedup set. Returns false
    // when the client has hung up (Write failed). PHI is never disclosed here.
    [[nodiscard]] bool emit_new_samples(grpc::ServerWriter<fmgr::v1::Sample>& writer,
                                        const std::vector<core::Sample>& samples,
                                        std::int64_t& cursor_micros,
                                        std::unordered_set<std::string>& sent_at_cursor) {
      std::int64_t max_at = cursor_micros;
      for (const auto& sample : samples) {
        const std::int64_t sample_at = sample.last_modified_at.unix_micros();
        const std::string id = sample.id.to_string();
        if (sample_at == cursor_micros && sent_at_cursor.contains(id)) {
          continue;
        }
        fmgr::v1::Sample out;
        fill_sample(&out, sample);
        if (!writer.Write(out)) {
          return false;
        }
        max_at = std::max(max_at, sample_at);
      }
      if (max_at > cursor_micros) {
        cursor_micros = max_at;
        sent_at_cursor.clear();
      }
      for (const auto& sample : samples) {
        if (sample.last_modified_at.unix_micros() == cursor_micros) {
          sent_at_cursor.insert(sample.id.to_string());
        }
      }
      return true;
    }

  } // namespace

  SampleServiceImpl::SampleServiceImpl(auth::IAuthProvider& auth, storage::IStorageBackend& backend,
                                       kms::IKmsProvider* kms)
      : auth_(auth), backend_(backend), kms_(kms), middleware_(auth) {
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
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.SampleService/ImportSamples", P::SampleWrite);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.SampleService/WatchSampleList", P::SampleRead);
  }

  grpc::Status SampleServiceImpl::ListSamples(grpc::ServerContext* ctx,
                                              const fmgr::v1::ListSamplesRequest* req,
                                              fmgr::v1::ListSamplesResponse* resp) {
    try {
      // Authenticate before touching request fields: a missing/invalid token must
      // surface as UNAUTHENTICATED, not as a downstream INTERNAL from parsing an
      // (unvalidated) empty lab_id.
      const auto bearer = extract_bearer(*ctx);
      const auto lab_id = core::LabId::parse(req->lab_id());
      const auto sctx = middleware_.authorize(bearer, core::Permission::SampleRead, lab_id);

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

      for (const auto& sample : samples) {
        auto* out = resp->add_samples();
        fill_sample(out, sample);
        const auto disclosed = reveal_phi(out, sample, sctx, kms_);
        if (!disclosed.empty()) {
          auto mut = make_ctx(*ctx, sctx, "list_samples");
          mut.lab_id = sample.lab_id.to_string();
          txn->note_phi_read("sample", sample.id.to_string(), mut, disclosed);
        }
      }
      txn->commit();
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

      // find_by_id returns tombstoned rows; treat them as not found (soft-delete).
      if (!sample.has_value() || sample->status == core::SampleStatus::Tombstoned) {
        return {grpc::StatusCode::NOT_FOUND, "sample not found"};
      }
      if (!sctx.has_for_lab(sample->lab_id, core::Permission::SampleRead)) {
        throw auth::PermissionDenied("sample.read required for this lab");
      }
      auto* out = resp->mutable_sample();
      fill_sample(out, *sample);
      const auto disclosed = reveal_phi(out, *sample, sctx, kms_);
      if (!disclosed.empty()) {
        auto mut = make_ctx(*ctx, sctx, "get_sample");
        mut.lab_id = sample->lab_id.to_string();
        txn->note_phi_read("sample", sample->id.to_string(), mut, disclosed);
      }
      txn->commit();
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
      const auto prepared =
          prepare_custom_fields(*txn, lab_id, item_type_id, req->custom_fields_json(), kms_);
      sample.custom_fields_json = prepared.custom_fields_json;
      sample.phi_fields_enc_json = prepared.phi_fields_enc_json;
      txn->repo<core::Sample>().insert(sample, make_ctx(*ctx, sctx, "create_sample"));
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
      const auto prepared =
          prepare_custom_fields(*txn, lab_id, item_type_id, wire.custom_fields_json(), kms_);
      existing->custom_fields_json = prepared.custom_fields_json;
      existing->phi_fields_enc_json = prepared.phi_fields_enc_json;
      existing->last_modified_by = sctx.user_id;
      existing->last_modified_at = now_timestamp();

      txn->repo<core::Sample>().update(*existing, make_ctx(*ctx, sctx, "update_sample"));
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
      txn->repo<core::Sample>().soft_delete(sample_id, make_ctx(*ctx, sctx, "soft_delete_sample"));
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
                                              make_ctx(*ctx, sctx, "move_sample"));
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
      const auto updated = storage::apply_checkout(*txn, sample_id, command,
                                                   make_ctx(*ctx, sctx, "checkout_sample"));
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
      // Authenticate before touching request fields: a missing/invalid token must
      // surface as UNAUTHENTICATED, not as a downstream INTERNAL from parsing an
      // (unvalidated) empty lab_id.
      const auto bearer = extract_bearer(*ctx);
      const auto lab_id = core::LabId::parse(req->lab_id());
      const auto sctx = middleware_.authorize(bearer, core::Permission::SampleRead, lab_id);

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

  grpc::Status SampleServiceImpl::ImportSamples(grpc::ServerContext* ctx,
                                                const fmgr::v1::ImportSamplesRequest* req,
                                                fmgr::v1::ImportSamplesResponse* resp) {
    try {
      // Authenticate before touching request fields (see ListSamples).
      const auto bearer = extract_bearer(*ctx);
      const auto lab_id = core::LabId::parse(req->lab_id());
      const auto sctx = middleware_.authorize(bearer, core::Permission::SampleWrite, lab_id);

      // Structural validation + row->Sample mapping, reusing the CLI importer core
      // (compiled into server_lib). lab/actor are server-supplied, never from the
      // CSV, so an import cannot smuggle rows into another lab or forge authorship.
      std::istringstream input(req->csv_content());
      const auto records = cli::parse_csv(input);
      const cli::ImportContext ictx{
          .lab_id = lab_id, .actor = sctx.user_id, .now = now_timestamp()};
      cli::ImportReport report = cli::build_import(records, ictx);

      if (!report.header_error.empty()) {
        resp->set_header_error(report.header_error);
        resp->set_committed(false);
        return grpc::Status::OK;
      }

      const bool any_structural_error =
          std::any_of(report.rows.begin(), report.rows.end(),
                      [](const cli::ImportRowResult& row) { return !row.ok; });

      // Dry-run, or a structural failure on a real import: validate/report only,
      // persist nothing.
      if (req->dry_run() || any_structural_error) {
        int succeeded = 0;
        int failed = 0;
        for (const auto& row : report.rows) {
          auto* out = resp->add_rows();
          out->set_row_number(static_cast<std::int32_t>(row.row_number));
          bool okay = row.ok;
          std::string error = row.error;
          // Dry-run additionally checks each structurally-ok row against committed
          // state (FK liveness, occupied position) in its own never-committed
          // transaction, mirroring `freezerctl sample import --dry-run`.
          if (req->dry_run() && okay) {
            // row.sample is guaranteed present when row.ok is true
            if (!row.sample.has_value()) {
              continue;
            }
            const auto& sample = *row.sample;
            try {
              auto probe = backend_.begin(storage::IsolationLevel::Serializable);
              rpc::AuthMiddleware::inject_rls_vars(*probe, sctx);
              probe->repo<core::Sample>().insert(sample,
                                                 make_ctx(*ctx, sctx, "import_samples_dryrun"));
              // Intentionally not committed: the transaction rolls back on scope exit.
            } catch (const std::exception& e) {
              okay = false;
              error = e.what();
            }
          }
          out->set_ok(okay);
          out->set_error(error);
          if (okay) {
            ++succeeded;
          } else {
            ++failed;
          }
        }
        resp->set_committed(false);
        resp->set_succeeded(succeeded);
        resp->set_failed(failed);
        return grpc::Status::OK;
      }

      // Real import, every row structurally valid: all-or-nothing in one
      // transaction. A storage failure (FK, occupied position, …) rolls the whole
      // batch back and surfaces as the mapped gRPC status — nothing is persisted.
      auto txn = backend_.begin(storage::IsolationLevel::Serializable);
      rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);
      for (const auto& row : report.rows) {
        // row.sample is guaranteed present when row.ok is true
        if (!row.sample.has_value()) {
          continue;
        }
        txn->repo<core::Sample>().insert(*row.sample, make_ctx(*ctx, sctx, "import_samples"));
      }
      txn->commit();

      for (const auto& row : report.rows) {
        auto* out = resp->add_rows();
        out->set_row_number(static_cast<std::int32_t>(row.row_number));
        out->set_ok(true);
        // row.sample is guaranteed present when row.ok is true
        if (row.sample.has_value()) {
          out->set_sample_id(row.sample->id.to_string());
        }
      }
      resp->set_committed(true);
      resp->set_succeeded(static_cast<std::int32_t>(report.rows.size()));
      resp->set_failed(0);
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

  grpc::Status SampleServiceImpl::WatchSampleList(grpc::ServerContext* ctx,
                                                  const fmgr::v1::WatchSampleListRequest* req,
                                                  grpc::ServerWriter<fmgr::v1::Sample>* writer) {
    try {
      // Authorize once at stream-open: a sample feed is always lab-scoped and
      // gates on SampleRead for that lab (held by every Member/ReadOnly), so a
      // missing/invalid token surfaces as UNAUTHENTICATED before parsing lab_id.
      const auto bearer = extract_bearer(*ctx);
      const auto lab_id = core::LabId::parse(req->lab_id());
      const auto sctx = middleware_.authorize(bearer, core::Permission::SampleRead, lab_id);

      // Poll-tail cursor on last_modified_at. Re-query rows with
      // last_modified_at >= cursor each cycle (>= not > so newly-changed rows
      // sharing the cursor microsecond are not missed) and suppress ids already
      // emitted at that microsecond via `sent_at_cursor`. No LISTEN/NOTIFY yet —
      // this stays portable across SQLite and Postgres. PHI is never disclosed.
      std::int64_t cursor_micros =
          req->has_since() ? req->since().unix_micros() : now_unix_micros();
      std::unordered_set<std::string> sent_at_cursor;

      while (!ctx->IsCancelled()) {
        auto txn = backend_.begin(storage::IsolationLevel::ReadCommitted);
        rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);
        const auto samples =
            txn->repo<core::Sample>().query(build_sample_watch_query(*req, lab_id, cursor_micros));
        txn->commit();

        if (!emit_new_samples(*writer, samples, cursor_micros, sent_at_cursor)) {
          return grpc::Status::OK; // client/gateway hung up
        }

        for (int i = 0; i < k_watch_poll_slices && !ctx->IsCancelled(); ++i) {
          std::this_thread::sleep_for(k_watch_poll_slice);
        }
      }
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

} // namespace fmgr::server
