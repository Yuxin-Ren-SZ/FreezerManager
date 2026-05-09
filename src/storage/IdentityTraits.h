// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_STORAGE_IDENTITYTRAITS_H
#define FMGR_STORAGE_IDENTITYTRAITS_H

#include "core/identity.h"
#include "storage/IStorageBackend.h"

#include <string_view>

namespace fmgr::storage {

  template <> struct EntityTraits<core::Lab> {
    using Id = core::Lab::Id;
    using Field = core::Lab::Field;

    [[nodiscard]] static constexpr std::string_view entity_name() {
      return "lab";
    }

    [[nodiscard]] static constexpr Field tombstone_field() {
      return Field::ArchivedAt;
    }
  };

  template <> struct EntityTraits<core::User> {
    using Id = core::User::Id;
    using Field = core::User::Field;

    [[nodiscard]] static constexpr std::string_view entity_name() {
      return "user";
    }

    [[nodiscard]] static constexpr Field tombstone_field() {
      return Field::Status;
    }
  };

  template <> struct EntityTraits<core::LabMembership> {
    using Id = core::LabMembership::Id;
    using Field = core::LabMembership::Field;

    [[nodiscard]] static constexpr std::string_view entity_name() {
      return "lab_membership";
    }

    [[nodiscard]] static constexpr Field tombstone_field() {
      return Field::RevokedAt;
    }
  };

} // namespace fmgr::storage

#endif // FMGR_STORAGE_IDENTITYTRAITS_H
