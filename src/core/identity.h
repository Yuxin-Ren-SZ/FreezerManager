// SPDX-License-Identifier: AGPL-3.0-or-later

// Core identity model: Lab (tenant), User (global identity), LabMembership (scoped grant).
// A `User` exists once globally and may belong to many `Lab`s simultaneously; each
// membership carries its own role and optional scope restrictions.
// All three entities use soft-delete (`archived_at` / `revoked_at`) so that audit
// history is never orphaned by a hard DELETE.
#ifndef FMGR_CORE_IDENTITY_H
#define FMGR_CORE_IDENTITY_H

#include "core/enums.h"
#include "core/ids.h"
#include "core/json_helpers.h"
#include "core/timestamp.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace fmgr::core {

  // Composite primary key encoded as `"{user_uuid}:{lab_uuid}"` for use in
  // audit log foreign-key columns and cross-entity references that need a
  // single stable string identifier.
  struct LabMembershipId {
    UserId user_id;
    LabId lab_id;

    [[nodiscard]] static LabMembershipId parse(std::string_view text) {
      const auto delimiter = text.find(':');
      if (delimiter == std::string_view::npos || delimiter == 0 || delimiter == text.size() - 1) {
        throw std::invalid_argument("invalid lab membership id");
      }
      return LabMembershipId{
          .user_id = UserId::parse(text.substr(0, delimiter)),
          .lab_id = LabId::parse(text.substr(delimiter + 1)),
      };
    }

    [[nodiscard]] std::string to_string() const {
      return user_id.to_string() + ":" + lab_id.to_string();
    }

    friend constexpr auto operator<=>(const LabMembershipId&, const LabMembershipId&) = default;
  };

  struct Lab {
    using Id = LabId;

    enum class Field : std::uint8_t {
      Id,
      Name,
      Contact,
      CreatedAt,
      SettingsJson,
      IsPhiEnabled,
      ArchivedAt
    };

    LabId id;
    std::string name;
    std::string contact;
    Timestamp created_at;
    nlohmann::json settings_json = nlohmann::json::object();
    bool is_phi_enabled{false};
    std::optional<Timestamp> archived_at;

    friend bool operator==(const Lab&, const Lab&) = default;
  };

  struct User {
    using Id = UserId;

    enum class Field : std::uint8_t {
      Id,
      PrimaryEmail,
      DisplayName,
      Status,
      CreatedAt,
      AuthBindings,
      TotpSecretEnc,
      DefaultLabId
    };

    UserId id;
    std::string primary_email;
    std::string display_name;
    UserStatus status{UserStatus::Active};
    Timestamp created_at;
    // JSON array of SSO/OAuth provider bindings, e.g. `[{"provider":"oidc","sub":"..."}]`.
    // Empty array means password-only authentication.
    nlohmann::json auth_bindings = nlohmann::json::array();
    // TOTP secret encrypted with the deployment KMS key. Never stored in plaintext.
    std::optional<std::string> totp_secret_enc;
    std::optional<LabId> default_lab_id;

    friend bool operator==(const User&, const User&) = default;
  };

  struct LabMembership {
    using Id = LabMembershipId;

    enum class Field : std::uint8_t {
      UserId,
      LabId,
      RoleId,
      ScopeFiltersJson,
      InvitedBy,
      JoinedAt,
      RevokedAt
    };

    UserId user_id;
    LabId lab_id;
    // Null during the invite-pending phase before the user accepts and a role is assigned.
    std::optional<RoleId> role_id;
    // Restricts the membership's effective permissions to a subset of resources.
    // Schema is validated by `validate_scope_filter()` in role.h.
    nlohmann::json scope_filters_json = nlohmann::json::object();
    std::optional<UserId> invited_by;
    Timestamp joined_at;
    // Soft-delete timestamp. Non-null = inactive membership. Rows are never
    // hard-deleted so that the audit log retains a complete access history.
    std::optional<Timestamp> revoked_at;

    [[nodiscard]] LabMembershipId id() const {
      return LabMembershipId{.user_id = user_id, .lab_id = lab_id};
    }

    friend bool operator==(const LabMembership&, const LabMembership&) = default;
  };

  inline void to_json(nlohmann::json& json, const LabMembershipId& membership_id) {
    json = membership_id.to_string();
  }

  inline void from_json(const nlohmann::json& json, LabMembershipId& membership_id) {
    membership_id = LabMembershipId::parse(json.get<std::string>());
  }

  inline void to_json(nlohmann::json& json, const Lab& lab) {
    json = nlohmann::json{
        {"id", lab.id},
        {"name", lab.name},
        {"contact", lab.contact},
        {"created_at", lab.created_at},
        {"settings_json", lab.settings_json},
        {"is_phi_enabled", lab.is_phi_enabled},
        {"archived_at", json_helpers::opt_to_json(lab.archived_at)},
    };
  }

  inline void from_json(const nlohmann::json& json, Lab& lab) {
    lab = Lab{
        .id = json.at("id").get<LabId>(),
        .name = json.at("name").get<std::string>(),
        .contact = json.at("contact").get<std::string>(),
        .created_at = json.at("created_at").get<Timestamp>(),
        .settings_json = json.at("settings_json"),
        .is_phi_enabled = json.at("is_phi_enabled").get<bool>(),
        .archived_at = json_helpers::opt_from_json<Timestamp>(json.at("archived_at")),
    };
  }

  inline void to_json(nlohmann::json& json, const User& user) {
    json = nlohmann::json{
        {"id", user.id},
        {"primary_email", user.primary_email},
        {"display_name", user.display_name},
        {"status", user.status},
        {"created_at", user.created_at},
        {"auth_bindings", user.auth_bindings},
        {"totp_secret_enc", json_helpers::opt_to_json(user.totp_secret_enc)},
        {"default_lab_id", json_helpers::opt_to_json(user.default_lab_id)},
    };
  }

  inline void from_json(const nlohmann::json& json, User& user) {
    user = User{
        .id = json.at("id").get<UserId>(),
        .primary_email = json.at("primary_email").get<std::string>(),
        .display_name = json.at("display_name").get<std::string>(),
        .status = json.at("status").get<UserStatus>(),
        .created_at = json.at("created_at").get<Timestamp>(),
        .auth_bindings = json.at("auth_bindings"),
        .totp_secret_enc = json_helpers::opt_from_json<std::string>(json.at("totp_secret_enc")),
        .default_lab_id = json_helpers::opt_from_json<LabId>(json.at("default_lab_id")),
    };
  }

  inline void to_json(nlohmann::json& json, const LabMembership& membership) {
    json = nlohmann::json{
        {"user_id", membership.user_id},
        {"lab_id", membership.lab_id},
        {"role_id", json_helpers::opt_to_json(membership.role_id)},
        {"scope_filters_json", membership.scope_filters_json},
        {"invited_by", json_helpers::opt_to_json(membership.invited_by)},
        {"joined_at", membership.joined_at},
        {"revoked_at", json_helpers::opt_to_json(membership.revoked_at)},
    };
  }

  inline void from_json(const nlohmann::json& json, LabMembership& membership) {
    membership = LabMembership{
        .user_id = json.at("user_id").get<UserId>(),
        .lab_id = json.at("lab_id").get<LabId>(),
        .role_id = json_helpers::opt_from_json<RoleId>(json.at("role_id")),
        .scope_filters_json = json.at("scope_filters_json"),
        .invited_by = json_helpers::opt_from_json<UserId>(json.at("invited_by")),
        .joined_at = json.at("joined_at").get<Timestamp>(),
        .revoked_at = json_helpers::opt_from_json<Timestamp>(json.at("revoked_at")),
    };
  }

} // namespace fmgr::core

#endif // FMGR_CORE_IDENTITY_H
