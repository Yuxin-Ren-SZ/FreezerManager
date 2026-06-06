// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_STORAGE_DETAIL_SAMPLECOLUMNS_H
#define FMGR_STORAGE_DETAIL_SAMPLECOLUMNS_H

#include "core/sample.h"
#include "storage/IStorageBackend.h"

#include <string>

// Backend-neutral column-name mapping and pure shape validation for the sample
// entities (Sample, Project, SampleProject, CheckoutEvent). Column names are
// identical across both schemas. The sample cross-entity checks (box / position /
// size-class / item-type liveness) need queries and stay in each backend's
// repository; only the pure shape rules live here.
namespace fmgr::storage::detail {

  [[nodiscard]] inline std::string sample_column_name(core::Sample::Field field) {
    switch (field) {
    case core::Sample::Field::Id:
      return "id";
    case core::Sample::Field::LabId:
      return "lab_id";
    case core::Sample::Field::ItemTypeId:
      return "item_type_id";
    case core::Sample::Field::Name:
      return "name";
    case core::Sample::Field::Barcode:
      return "barcode";
    case core::Sample::Field::ContainerTypeId:
      return "container_type_id";
    case core::Sample::Field::BoxId:
      return "box_id";
    case core::Sample::Field::PositionLabel:
      return "position_label";
    case core::Sample::Field::VolumeValue:
      return "volume_value";
    case core::Sample::Field::VolumeUnit:
      return "volume_unit";
    case core::Sample::Field::MassValue:
      return "mass_value";
    case core::Sample::Field::MassUnit:
      return "mass_unit";
    case core::Sample::Field::Status:
      return "status";
    case core::Sample::Field::ParentSampleId:
      return "parent_sample_id";
    case core::Sample::Field::CreatedBy:
      return "created_by";
    case core::Sample::Field::CreatedAt:
      return "created_at_micros";
    case core::Sample::Field::LastModifiedBy:
      return "last_modified_by";
    case core::Sample::Field::LastModifiedAt:
      return "last_modified_at_micros";
    case core::Sample::Field::CustomFieldsJson:
      return "custom_fields_json";
    case core::Sample::Field::PhiFieldsEncJson:
      return "phi_fields_enc_json";
    }
    throw ConstraintViolation("unknown sample field");
  }

  [[nodiscard]] inline std::string project_column_name(core::Project::Field field) {
    switch (field) {
    case core::Project::Field::Id:
      return "id";
    case core::Project::Field::LabId:
      return "lab_id";
    case core::Project::Field::Name:
      return "name";
    case core::Project::Field::OwnerUserId:
      return "owner_user_id";
    case core::Project::Field::CreatedAt:
      return "created_at_micros";
    case core::Project::Field::ArchivedAt:
      return "archived_at_micros";
    }
    throw ConstraintViolation("unknown project field");
  }

  [[nodiscard]] inline std::string sample_project_column_name(core::SampleProject::Field field) {
    switch (field) {
    case core::SampleProject::Field::SampleId:
      return "sample_id";
    case core::SampleProject::Field::ProjectId:
      return "project_id";
    }
    throw ConstraintViolation("unknown sample_project field");
  }

  [[nodiscard]] inline std::string checkout_event_column_name(core::CheckoutEvent::Field field) {
    switch (field) {
    case core::CheckoutEvent::Field::Id:
      return "id";
    case core::CheckoutEvent::Field::SampleId:
      return "sample_id";
    case core::CheckoutEvent::Field::LabId:
      return "lab_id";
    case core::CheckoutEvent::Field::UserId:
      return "user_id";
    case core::CheckoutEvent::Field::Action:
      return "action";
    case core::CheckoutEvent::Field::Reason:
      return "reason";
    case core::CheckoutEvent::Field::At:
      return "at_micros";
    case core::CheckoutEvent::Field::VolumeDelta:
      return "volume_delta";
    case core::CheckoutEvent::Field::VolumeUnit:
      return "volume_unit";
    case core::CheckoutEvent::Field::LocationAfter:
      return "location_after";
    }
    throw ConstraintViolation("unknown checkout_event field");
  }

  // Pure shape validation for a Sample (name present, box/position paired). The
  // box, position-label, size-class and item-type cross-references are enforced
  // by each backend's repository against committed rows.
  inline void validate_sample_shape(const core::Sample& sample) {
    if (sample.name.empty()) {
      throw ConstraintViolation("sample name is required");
    }
    if (sample.box_id.has_value() != sample.position_label.has_value()) {
      throw ConstraintViolation("box_id and position_label must both be set or both null");
    }
  }

  inline void validate_project(const core::Project& project) {
    if (project.name.empty()) {
      throw ConstraintViolation("project name is required");
    }
  }

} // namespace fmgr::storage::detail

#endif // FMGR_STORAGE_DETAIL_SAMPLECOLUMNS_H
