// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_CORE_ROLE_H
#define FMGR_CORE_ROLE_H

#include "core/enums.h"
#include "core/identity.h"
#include "core/ids.h"
#include "core/json_helpers.h"
#include "core/permissions.h"
#include "core/timestamp.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace fmgr::core {

  struct RolePermissionId {
    RoleId role_id;
    Permission permission;

    [[nodiscard]] std::string to_string() const {
      return role_id.to_string() + ":" + std::string(to_key(permission));
    }

    friend constexpr auto operator<=>(const RolePermissionId&, const RolePermissionId&) = default;
  };

  struct Role {
    using Id = RoleId;

    enum class Field : std::uint8_t {
      Id,
      LabId,
      Kind,
      Name,
      Description,
      IsBuiltin,
      CreatedAt,
      ArchivedAt,
    };

    RoleId id;
    std::optional<LabId> lab_id;
    RoleKind kind{RoleKind::Member};
    std::string name;
    std::string description;
    bool is_builtin{false};
    Timestamp created_at;
    std::optional<Timestamp> archived_at;

    friend bool operator==(const Role&, const Role&) = default;
  };

  struct RolePermission {
    using Id = RolePermissionId;

    enum class Field : std::uint8_t {
      RoleId,
      PermissionKey,
    };

    RoleId role_id;
    Permission permission{Permission::SampleRead};

    [[nodiscard]] RolePermissionId id() const {
      return RolePermissionId{.role_id = role_id, .permission = permission};
    }

    friend bool operator==(const RolePermission&, const RolePermission&) = default;
  };

  // Built-in role UUIDs are reserved in the 00000000-0000-0000-0000-00000000000X
  // namespace and are duplicated verbatim in the SQLite seed migration so that
  // SQLite and (future) Postgres deployments produce identical role rows.
  [[nodiscard]] inline RoleId builtin_role_id(RoleKind kind) {
    switch (kind) {
    case RoleKind::SystemAdmin:
      return RoleId::parse("00000000-0000-0000-0000-000000000001");
    case RoleKind::LabAdmin:
      return RoleId::parse("00000000-0000-0000-0000-000000000002");
    case RoleKind::Member:
      return RoleId::parse("00000000-0000-0000-0000-000000000003");
    case RoleKind::ReadOnly:
      return RoleId::parse("00000000-0000-0000-0000-000000000004");
    case RoleKind::ApiClient:
      return RoleId::parse("00000000-0000-0000-0000-000000000005");
    }
    throw std::invalid_argument("unknown role kind");
  }

  // Schema for `LabMembership.scope_filters_json`. Empty object = unrestricted
  // within the role's grant. Each key is an additive whitelist; the membership
  // can write only to entries appearing in every present whitelist (the field
  // narrows, never broadens). Validator rejects unknown keys so a typo doesn't
  // silently disable a restriction.
  inline constexpr std::array<std::string_view, 3> k_scope_filter_keys{
      "freezer_in",
      "project_in",
      "item_type_in",
  };

  inline void validate_scope_filter(const nlohmann::json& filter) {
    if (!filter.is_object()) {
      throw std::invalid_argument("scope filter must be a JSON object");
    }
    for (const auto& [key, value] : filter.items()) {
      bool recognized = false;
      for (const auto known : k_scope_filter_keys) {
        if (key == known) {
          recognized = true;
          break;
        }
      }
      if (!recognized) {
        throw std::invalid_argument("scope filter has unknown key: " + key);
      }
      if (!value.is_array()) {
        throw std::invalid_argument("scope filter value for '" + key + "' must be an array");
      }
      for (const auto& element : value) {
        if (!element.is_string()) {
          throw std::invalid_argument("scope filter '" + key + "' must contain only string IDs");
        }
      }
    }
  }

  inline void to_json(nlohmann::json& json, const RolePermissionId& grant_id) {
    json = grant_id.to_string();
  }

  inline void from_json(const nlohmann::json& json, RolePermissionId& grant_id) {
    const auto text = json.get<std::string>();
    const auto delimiter = text.find(':');
    if (delimiter == std::string::npos || delimiter == 0 || delimiter == text.size() - 1) {
      throw std::invalid_argument("invalid role permission id");
    }
    grant_id = RolePermissionId{
        .role_id = RoleId::parse(text.substr(0, delimiter)),
        .permission = parse_permission(text.substr(delimiter + 1)),
    };
  }

  inline void to_json(nlohmann::json& json, const Role& role) {
    json = nlohmann::json{
        {"id", role.id},
        {"lab_id", json_helpers::opt_to_json(role.lab_id)},
        {"kind", role.kind},
        {"name", role.name},
        {"description", role.description},
        {"is_builtin", role.is_builtin},
        {"created_at", role.created_at},
        {"archived_at", json_helpers::opt_to_json(role.archived_at)},
    };
  }

  inline void from_json(const nlohmann::json& json, Role& role) {
    role = Role{
        .id = json.at("id").get<RoleId>(),
        .lab_id = json_helpers::opt_from_json<LabId>(json.at("lab_id")),
        .kind = json.at("kind").get<RoleKind>(),
        .name = json.at("name").get<std::string>(),
        .description = json.at("description").get<std::string>(),
        .is_builtin = json.at("is_builtin").get<bool>(),
        .created_at = json.at("created_at").get<Timestamp>(),
        .archived_at = json_helpers::opt_from_json<Timestamp>(json.at("archived_at")),
    };
  }

  inline void to_json(nlohmann::json& json, const RolePermission& grant) {
    json = nlohmann::json{
        {"role_id", grant.role_id},
        {"permission", grant.permission},
    };
  }

  inline void from_json(const nlohmann::json& json, RolePermission& grant) {
    grant = RolePermission{
        .role_id = json.at("role_id").get<RoleId>(),
        .permission = json.at("permission").get<Permission>(),
    };
  }

} // namespace fmgr::core

#endif // FMGR_CORE_ROLE_H
