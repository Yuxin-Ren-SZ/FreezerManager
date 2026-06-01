// SPDX-License-Identifier: AGPL-3.0-or-later

// D7: Sample lifecycle, Project grouping, SampleProject links, CheckoutEvent audit.
// Sample is the central entity: a physical specimen tracked to a position-in-box.
// Tombstone: status = tombstoned (not archived_at_micros). The row is retained for
// audit continuity but excluded from default queries.
// CheckoutEvent is append-only; rows are never updated or soft-deleted.
#ifndef FMGR_CORE_SAMPLE_H
#define FMGR_CORE_SAMPLE_H

#include "core/enums.h"
#include "core/ids.h"
#include "core/quantity.h"
#include "core/timestamp.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

namespace fmgr::core {

  namespace detail {

    template <typename Value>
    [[nodiscard]] inline nlohmann::json sample_opt_to_json(const std::optional<Value>& value) {
      if (!value.has_value()) {
        return nullptr;
      }
      return nlohmann::json(value.value());
    }

    template <typename Value>
    [[nodiscard]] inline std::optional<Value> sample_opt_from_json(const nlohmann::json& json) {
      if (json.is_null()) {
        return std::nullopt;
      }
      return json.get<Value>();
    }

  } // namespace detail

  // ---- VolumeUnit / MassUnit JSON converters ----
  // (to_string / parse_* are in quantity.h; JSON adapters live here
  //  because Sample is the only entity that persists them individually.)

  inline void to_json(nlohmann::json& json, VolumeUnit unit) {
    json = std::string(to_string(unit));
  }
  inline void from_json(const nlohmann::json& json, VolumeUnit& unit) {
    unit = parse_volume_unit(json.get<std::string>());
  }

  inline void to_json(nlohmann::json& json, MassUnit unit) {
    json = std::string(to_string(unit));
  }
  inline void from_json(const nlohmann::json& json, MassUnit& unit) {
    unit = parse_mass_unit(json.get<std::string>());
  }

  // ---- Project ----

  // Optional grouping of samples within a lab. Soft-deleted via archived_at_micros.
  struct Project {
    using Id = ProjectId;

    enum class Field : std::uint8_t { Id, LabId, Name, OwnerUserId, CreatedAt, ArchivedAt };

    ProjectId id;
    LabId lab_id;
    std::string name;
    UserId owner_user_id;
    Timestamp created_at;
    std::optional<Timestamp> archived_at;

    friend bool operator==(const Project&, const Project&) = default;
  };

  // ---- SampleProject ----

  // Composite primary key for the sample_projects many-to-many link table.
  struct SampleProjectId {
    SampleId sample_id;
    ProjectId project_id;

    friend constexpr auto operator<=>(const SampleProjectId&, const SampleProjectId&) = default;
    friend constexpr bool operator==(const SampleProjectId&, const SampleProjectId&) = default;
  };

  // Membership link between a Sample and a Project.
  // Links are hard-deleted when removed; no archived_at column.
  struct SampleProject {
    using Id = SampleProjectId;

    enum class Field : std::uint8_t { SampleId, ProjectId };

    SampleId sample_id;
    ProjectId project_id;

    [[nodiscard]] SampleProjectId id() const {
      return SampleProjectId{sample_id, project_id};
    }

    friend bool operator==(const SampleProject&, const SampleProject&) = default;
  };

  // ---- CheckoutEvent ----

  // Immutable chain-of-custody record for every sample lifecycle transition.
  // Once inserted these rows are never updated or soft-deleted (append-only audit).
  // volume_delta (raw integer in volume_unit) captures quantity change; negative = consumed.
  struct CheckoutEvent {
    using Id = CheckoutEventId;

    enum class Field : std::uint8_t {
      Id,
      SampleId,
      LabId,
      UserId,
      Action,
      Reason,
      At,
      VolumeDelta,
      VolumeUnit,
      LocationAfter,
    };

    CheckoutEventId id;
    SampleId sample_id;
    LabId lab_id;
    UserId user_id;
    CheckoutAction action;
    std::optional<std::string> reason;
    Timestamp at;
    std::optional<std::int64_t> volume_delta;
    std::optional<core::VolumeUnit> volume_unit;
    std::optional<std::string> location_after;

    friend bool operator==(const CheckoutEvent&, const CheckoutEvent&) = default;
  };

  // ---- Sample ----

  // The central entity. Tracks a physical specimen down to a position-in-box.
  //
  // Tombstone: status = tombstoned (not archived_at_micros).
  // Position invariant: box_id and position_label must both be null or both non-null.
  // No-double-booking: DB partial unique index on (box_id, position_label)
  //   WHERE status IN ('active', 'checked_out').
  // PHI: phi_fields_enc_json holds per-field AEAD-encrypted data (Section H);
  //   custom_fields_json holds non-PHI fields validated at the RPC layer.
  struct Sample {
    using Id = SampleId;

    enum class Field : std::uint8_t {
      Id,
      LabId,
      ItemTypeId,
      Name,
      Barcode,
      ContainerTypeId,
      BoxId,
      PositionLabel,
      VolumeValue,
      VolumeUnit,
      MassValue,
      MassUnit,
      Status,
      ParentSampleId,
      CreatedBy,
      CreatedAt,
      LastModifiedBy,
      LastModifiedAt,
      CustomFieldsJson,
      PhiFieldsEncJson,
    };

    SampleId id;
    LabId lab_id;
    ItemTypeId item_type_id;
    std::string name;
    std::optional<std::string> barcode;
    std::optional<ContainerTypeId> container_type_id;
    std::optional<BoxId> box_id;
    std::optional<std::string> position_label; // must be set iff box_id is set
    std::optional<std::int64_t> volume_value;  // raw integer in volume_unit
    std::optional<core::VolumeUnit> volume_unit;
    std::optional<std::int64_t> mass_value; // raw integer in mass_unit
    std::optional<core::MassUnit> mass_unit;
    SampleStatus status{SampleStatus::Active};
    std::optional<SampleId> parent_sample_id; // aliquot lineage; preserved through soft-delete
    UserId created_by;
    Timestamp created_at;
    UserId last_modified_by;
    Timestamp last_modified_at;
    std::string custom_fields_json{"{}"};  // validated against CustomFieldDefinition at RPC layer
    std::string phi_fields_enc_json{"{}"}; // per-field AEAD-encrypted PHI (Section H)

    friend bool operator==(const Sample&, const Sample&) = default;
  };

  // ---- JSON serialization ----

  inline void to_json(nlohmann::json& json, const Project& project) {
    json = nlohmann::json{
        {"id", project.id},
        {"lab_id", project.lab_id},
        {"name", project.name},
        {"owner_user_id", project.owner_user_id},
        {"created_at", project.created_at},
        {"archived_at", detail::sample_opt_to_json(project.archived_at)},
    };
  }

  inline void from_json(const nlohmann::json& json, Project& project) {
    project = Project{
        .id = json.at("id").get<ProjectId>(),
        .lab_id = json.at("lab_id").get<LabId>(),
        .name = json.at("name").get<std::string>(),
        .owner_user_id = json.at("owner_user_id").get<UserId>(),
        .created_at = json.at("created_at").get<Timestamp>(),
        .archived_at = detail::sample_opt_from_json<Timestamp>(json.at("archived_at")),
    };
  }

  inline void to_json(nlohmann::json& json, const SampleProjectId& sp_id) {
    json = nlohmann::json{
        {"sample_id", sp_id.sample_id},
        {"project_id", sp_id.project_id},
    };
  }

  inline void from_json(const nlohmann::json& json, SampleProjectId& sp_id) {
    sp_id = SampleProjectId{
        .sample_id = json.at("sample_id").get<SampleId>(),
        .project_id = json.at("project_id").get<ProjectId>(),
    };
  }

  inline void to_json(nlohmann::json& json, const SampleProject& sp) {
    json = nlohmann::json{
        {"sample_id", sp.sample_id},
        {"project_id", sp.project_id},
    };
  }

  inline void from_json(const nlohmann::json& json, SampleProject& sp) {
    sp = SampleProject{
        .sample_id = json.at("sample_id").get<SampleId>(),
        .project_id = json.at("project_id").get<ProjectId>(),
    };
  }

  inline void to_json(nlohmann::json& json, const CheckoutEvent& event) {
    json = nlohmann::json{
        {"id", event.id},
        {"sample_id", event.sample_id},
        {"lab_id", event.lab_id},
        {"user_id", event.user_id},
        {"action", event.action},
        {"reason", detail::sample_opt_to_json(event.reason)},
        {"at", event.at},
        {"volume_delta", detail::sample_opt_to_json(event.volume_delta)},
        {"volume_unit", detail::sample_opt_to_json(event.volume_unit)},
        {"location_after", detail::sample_opt_to_json(event.location_after)},
    };
  }

  inline void from_json(const nlohmann::json& json, CheckoutEvent& event) {
    event = CheckoutEvent{
        .id = json.at("id").get<CheckoutEventId>(),
        .sample_id = json.at("sample_id").get<SampleId>(),
        .lab_id = json.at("lab_id").get<LabId>(),
        .user_id = json.at("user_id").get<UserId>(),
        .action = json.at("action").get<CheckoutAction>(),
        .reason = detail::sample_opt_from_json<std::string>(json.at("reason")),
        .at = json.at("at").get<Timestamp>(),
        .volume_delta = detail::sample_opt_from_json<std::int64_t>(json.at("volume_delta")),
        .volume_unit = detail::sample_opt_from_json<VolumeUnit>(json.at("volume_unit")),
        .location_after = detail::sample_opt_from_json<std::string>(json.at("location_after")),
    };
  }

  inline void to_json(nlohmann::json& json, const Sample& sample) {
    json = nlohmann::json{
        {"id", sample.id},
        {"lab_id", sample.lab_id},
        {"item_type_id", sample.item_type_id},
        {"name", sample.name},
        {"barcode", detail::sample_opt_to_json(sample.barcode)},
        {"container_type_id", detail::sample_opt_to_json(sample.container_type_id)},
        {"box_id", detail::sample_opt_to_json(sample.box_id)},
        {"position_label", detail::sample_opt_to_json(sample.position_label)},
        {"volume_value", detail::sample_opt_to_json(sample.volume_value)},
        {"volume_unit", detail::sample_opt_to_json(sample.volume_unit)},
        {"mass_value", detail::sample_opt_to_json(sample.mass_value)},
        {"mass_unit", detail::sample_opt_to_json(sample.mass_unit)},
        {"status", sample.status},
        {"parent_sample_id", detail::sample_opt_to_json(sample.parent_sample_id)},
        {"created_by", sample.created_by},
        {"created_at", sample.created_at},
        {"last_modified_by", sample.last_modified_by},
        {"last_modified_at", sample.last_modified_at},
        {"custom_fields_json", sample.custom_fields_json},
        {"phi_fields_enc_json", sample.phi_fields_enc_json},
    };
  }

  inline void from_json(const nlohmann::json& json, Sample& sample) {
    sample = Sample{
        .id = json.at("id").get<SampleId>(),
        .lab_id = json.at("lab_id").get<LabId>(),
        .item_type_id = json.at("item_type_id").get<ItemTypeId>(),
        .name = json.at("name").get<std::string>(),
        .barcode = detail::sample_opt_from_json<std::string>(json.at("barcode")),
        .container_type_id =
            detail::sample_opt_from_json<ContainerTypeId>(json.at("container_type_id")),
        .box_id = detail::sample_opt_from_json<BoxId>(json.at("box_id")),
        .position_label = detail::sample_opt_from_json<std::string>(json.at("position_label")),
        .volume_value = detail::sample_opt_from_json<std::int64_t>(json.at("volume_value")),
        .volume_unit = detail::sample_opt_from_json<VolumeUnit>(json.at("volume_unit")),
        .mass_value = detail::sample_opt_from_json<std::int64_t>(json.at("mass_value")),
        .mass_unit = detail::sample_opt_from_json<MassUnit>(json.at("mass_unit")),
        .status = json.at("status").get<SampleStatus>(),
        .parent_sample_id = detail::sample_opt_from_json<SampleId>(json.at("parent_sample_id")),
        .created_by = json.at("created_by").get<UserId>(),
        .created_at = json.at("created_at").get<Timestamp>(),
        .last_modified_by = json.at("last_modified_by").get<UserId>(),
        .last_modified_at = json.at("last_modified_at").get<Timestamp>(),
        .custom_fields_json = json.at("custom_fields_json").get<std::string>(),
        .phi_fields_enc_json = json.at("phi_fields_enc_json").get<std::string>(),
    };
  }

} // namespace fmgr::core

#endif // FMGR_CORE_SAMPLE_H
