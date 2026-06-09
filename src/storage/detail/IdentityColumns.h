// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_STORAGE_DETAIL_IDENTITYCOLUMNS_H
#define FMGR_STORAGE_DETAIL_IDENTITYCOLUMNS_H

#include "core/identity.h"
#include "storage/IStorageBackend.h"

#include <string>
#include <string_view>

// Backend-neutral column-name mapping and pure validation for the identity
// entities (Lab, User, LabMembership). The column names are identical across the
// SQLite and PostgreSQL schemas (the migrations mirror each other), so both
// backends share these. Validation here is pure (no driver calls); cross-entity
// integrity that needs a query stays in each backend's repository.
namespace fmgr::storage::detail {

  [[nodiscard]] inline std::string lab_column_name(core::Lab::Field field) {
    switch (field) {
    case core::Lab::Field::Id:
      return "id";
    case core::Lab::Field::Name:
      return "name";
    case core::Lab::Field::Contact:
      return "contact";
    case core::Lab::Field::CreatedAt:
      return "created_at_micros";
    case core::Lab::Field::SettingsJson:
      return "settings_json";
    case core::Lab::Field::IsPhiEnabled:
      return "is_phi_enabled";
    case core::Lab::Field::ArchivedAt:
      return "archived_at_micros";
    }
    throw ConstraintViolation("unknown lab field");
  }

  [[nodiscard]] inline std::string user_column_name(core::User::Field field) {
    switch (field) {
    case core::User::Field::Id:
      return "id";
    case core::User::Field::PrimaryEmail:
      return "primary_email";
    case core::User::Field::DisplayName:
      return "display_name";
    case core::User::Field::Status:
      return "status";
    case core::User::Field::CreatedAt:
      return "created_at_micros";
    case core::User::Field::AuthBindings:
      return "auth_bindings_json";
    case core::User::Field::TotpSecretEnc:
      return "totp_secret_enc";
    case core::User::Field::DefaultLabId:
      return "default_lab_id";
    case core::User::Field::AuthzVersion:
      return "authz_version";
    }
    throw ConstraintViolation("unknown user field");
  }

  [[nodiscard]] inline std::string membership_column_name(core::LabMembership::Field field) {
    switch (field) {
    case core::LabMembership::Field::UserId:
      return "user_id";
    case core::LabMembership::Field::LabId:
      return "lab_id";
    case core::LabMembership::Field::RoleId:
      return "role_id";
    case core::LabMembership::Field::ScopeFiltersJson:
      return "scope_filters_json";
    case core::LabMembership::Field::InvitedBy:
      return "invited_by";
    case core::LabMembership::Field::JoinedAt:
      return "joined_at_micros";
    case core::LabMembership::Field::RevokedAt:
      return "revoked_at_micros";
    }
    throw ConstraintViolation("unknown lab membership field");
  }

  [[nodiscard]] inline bool looks_like_email(std::string_view email) {
    const auto delimiter = email.find('@');
    return delimiter != std::string_view::npos && delimiter != 0 && delimiter != email.size() - 1;
  }

  inline void validate_lab(const core::Lab& lab) {
    if (lab.name.empty()) {
      throw ConstraintViolation("lab name is required");
    }
    if (!lab.settings_json.is_object()) {
      throw ConstraintViolation("lab settings_json must be an object");
    }
  }

  inline void validate_user(const core::User& user) {
    if (!looks_like_email(user.primary_email)) {
      throw ConstraintViolation("user primary_email is invalid");
    }
    if (user.display_name.empty()) {
      throw ConstraintViolation("user display_name is required");
    }
    if (!user.auth_bindings.is_array()) {
      throw ConstraintViolation("user auth_bindings must be an array");
    }
  }

  inline void validate_membership(const core::LabMembership& membership) {
    if (!membership.scope_filters_json.is_object()) {
      throw ConstraintViolation("lab membership scope_filters_json must be an object");
    }
  }

} // namespace fmgr::storage::detail

#endif // FMGR_STORAGE_DETAIL_IDENTITYCOLUMNS_H
