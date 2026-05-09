// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_STORAGE_ROLETRAITS_H
#define FMGR_STORAGE_ROLETRAITS_H

#include "core/role.h"
#include "storage/IStorageBackend.h"

#include <string_view>

namespace fmgr::storage {

  template <> struct EntityTraits<core::Role> {
    using Id = core::Role::Id;
    using Field = core::Role::Field;

    [[nodiscard]] static constexpr std::string_view entity_name() {
      return "role";
    }

    [[nodiscard]] static constexpr Field tombstone_field() {
      return Field::ArchivedAt;
    }
  };

  // RolePermission rows are pure relationships and do not carry a tombstone.
  // soft_delete() on this repository performs a hard DELETE while still
  // appending an audit row through the transaction's mutation hook.
  template <> struct EntityTraits<core::RolePermission> {
    using Id = core::RolePermission::Id;
    using Field = core::RolePermission::Field;

    [[nodiscard]] static constexpr std::string_view entity_name() {
      return "role_permission";
    }

    [[nodiscard]] static constexpr Field tombstone_field() {
      return Field::RoleId;
    }
  };

} // namespace fmgr::storage

#endif // FMGR_STORAGE_ROLETRAITS_H
