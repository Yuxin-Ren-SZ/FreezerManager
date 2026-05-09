// SPDX-License-Identifier: AGPL-3.0-or-later

// Single source of truth for the FreezerManager permission catalog.
// Every RPC handler annotates the permission(s) it requires by `Permission`
// enumerator; the SQLite seed (migration 0003) inserts one row per
// `to_key(p)` for every `p` in `all_permissions()`.
#ifndef FMGR_CORE_PERMISSIONS_H
#define FMGR_CORE_PERMISSIONS_H

#include "core/enums.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>

namespace fmgr::core {

  enum class Permission : std::uint8_t {
    SampleRead,
    SampleWrite,
    SampleCheckout,
    SampleDeleteSoft,
    SampleDeleteHard,
    BoxConfigure,
    FreezerConfigure,
    CustomFieldDefine,
    ItemTypeDefine,
    UserInvite,
    UserManageRoles,
    AuditRead,
    AuditExport,
    BackupRun,
    ShareRequest,
    ShareApprove,
    PhiRead,
    LabConfigure,
    LabEnablePhi,
    KeyRotate,
    SessionRevoke,
  };

  struct PermissionEntry {
    Permission value;
    std::string_view key;
    std::string_view description;
  };

  inline constexpr std::array<PermissionEntry, 21> k_permission_catalog{{
      {.value = Permission::SampleRead,
       .key = "sample.read",
       .description = "Read sample records."},
      {.value = Permission::SampleWrite,
       .key = "sample.write",
       .description = "Create or modify samples."},
      {.value = Permission::SampleCheckout,
       .key = "sample.checkout",
       .description = "Check samples in or out."},
      {.value = Permission::SampleDeleteSoft,
       .key = "sample.delete_soft",
       .description = "Tombstone a sample."},
      {.value = Permission::SampleDeleteHard,
       .key = "sample.delete_hard",
       .description = "Permanently delete tombstoned sample (SystemAdmin only)."},
      {.value = Permission::BoxConfigure,
       .key = "box.configure",
       .description = "Define or modify boxes and box types."},
      {.value = Permission::FreezerConfigure,
       .key = "freezer.configure",
       .description = "Configure freezers and storage layout."},
      {.value = Permission::CustomFieldDefine,
       .key = "custom_field.define",
       .description = "Define custom field schemas."},
      {.value = Permission::ItemTypeDefine,
       .key = "item_type.define",
       .description = "Define item-type taxonomy."},
      {.value = Permission::UserInvite,
       .key = "user.invite",
       .description = "Invite or revoke lab members."},
      {.value = Permission::UserManageRoles,
       .key = "user.manage_roles",
       .description = "Assign roles to lab members."},
      {.value = Permission::AuditRead,
       .key = "audit.read",
       .description = "Browse audit log entries."},
      {.value = Permission::AuditExport,
       .key = "audit.export",
       .description = "Export signed audit log files."},
      {.value = Permission::BackupRun,
       .key = "backup.run",
       .description = "Trigger a backup or restore-drill."},
      {.value = Permission::ShareRequest,
       .key = "share.request",
       .description = "Open a cross-lab share request."},
      {.value = Permission::ShareApprove,
       .key = "share.approve",
       .description = "Approve or reject a share request."},
      {.value = Permission::PhiRead,
       .key = "phi.read",
       .description = "Read PHI-tagged custom fields."},
      {.value = Permission::LabConfigure,
       .key = "lab.configure",
       .description = "Modify lab settings."},
      {.value = Permission::LabEnablePhi,
       .key = "lab.enable_phi",
       .description = "Toggle PHI mode on a lab."},
      {.value = Permission::KeyRotate,
       .key = "key.rotate",
       .description = "Rotate cryptographic keys."},
      {.value = Permission::SessionRevoke,
       .key = "session.revoke",
       .description = "Revoke another user's sessions."},
  }};

  [[nodiscard]] inline std::string_view to_key(Permission permission) {
    for (const auto& entry : k_permission_catalog) {
      if (entry.value == permission) {
        return entry.key;
      }
    }
    throw std::invalid_argument("unknown permission");
  }

  [[nodiscard]] inline std::string_view describe(Permission permission) {
    for (const auto& entry : k_permission_catalog) {
      if (entry.value == permission) {
        return entry.description;
      }
    }
    throw std::invalid_argument("unknown permission");
  }

  [[nodiscard]] inline Permission parse_permission(std::string_view key) {
    for (const auto& entry : k_permission_catalog) {
      if (entry.key == key) {
        return entry.value;
      }
    }
    throw std::invalid_argument("unknown permission key: " + std::string(key));
  }

  [[nodiscard]] inline std::set<Permission> all_permissions() {
    std::set<Permission> result;
    for (const auto& entry : k_permission_catalog) {
      result.insert(entry.value);
    }
    return result;
  }

  // Built-in permission grants per role. Custom roles get whatever the lab
  // admin selects; these defaults seed the five built-in roles in the DB
  // and document the safe baseline for every deployment.
  [[nodiscard]] inline std::set<Permission> builtin_role_permissions(RoleKind kind) {
    switch (kind) {
    case RoleKind::SystemAdmin:
      // Full deployment control. PhiRead is intentionally excluded by
      // default: a SystemAdmin must be granted PHI access through a
      // separately tracked role assignment so PHI access is loud in audit.
      return {
          Permission::SampleRead,       Permission::SampleWrite,       Permission::SampleCheckout,
          Permission::SampleDeleteSoft, Permission::SampleDeleteHard,  Permission::BoxConfigure,
          Permission::FreezerConfigure, Permission::CustomFieldDefine, Permission::ItemTypeDefine,
          Permission::UserInvite,       Permission::UserManageRoles,   Permission::AuditRead,
          Permission::AuditExport,      Permission::BackupRun,         Permission::ShareRequest,
          Permission::ShareApprove,     Permission::LabConfigure,      Permission::LabEnablePhi,
          Permission::KeyRotate,        Permission::SessionRevoke,
      };
    case RoleKind::LabAdmin:
      return {
          Permission::SampleRead,        Permission::SampleWrite,    Permission::SampleCheckout,
          Permission::SampleDeleteSoft,  Permission::BoxConfigure,   Permission::FreezerConfigure,
          Permission::CustomFieldDefine, Permission::ItemTypeDefine, Permission::UserInvite,
          Permission::UserManageRoles,   Permission::AuditRead,      Permission::AuditExport,
          Permission::BackupRun,         Permission::ShareRequest,   Permission::ShareApprove,
          Permission::LabConfigure,
      };
    case RoleKind::Member:
      return {
          Permission::SampleRead,       Permission::SampleWrite,  Permission::SampleCheckout,
          Permission::SampleDeleteSoft, Permission::ShareRequest,
      };
    case RoleKind::ReadOnly:
      return {
          Permission::SampleRead,
          Permission::AuditRead,
      };
    case RoleKind::ApiClient:
      // Conservative default: read + checkout. Operators can mint custom
      // roles for higher-privilege machine clients.
      return {
          Permission::SampleRead,
          Permission::SampleCheckout,
      };
    }
    throw std::invalid_argument("unknown role kind");
  }

  inline void to_json(nlohmann::json& json, Permission permission) {
    json = std::string(to_key(permission));
  }

  inline void from_json(const nlohmann::json& json, Permission& permission) {
    permission = parse_permission(json.get<std::string>());
  }

} // namespace fmgr::core

#endif // FMGR_CORE_PERMISSIONS_H
