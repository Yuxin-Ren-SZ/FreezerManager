// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_STORAGE_FREEZERTRAITS_H
#define FMGR_STORAGE_FREEZERTRAITS_H

#include "core/freezer.h"
#include "storage/IStorageBackend.h"

#include <string_view>

namespace fmgr::storage {

  template <> struct EntityTraits<core::Freezer> {
    using Id = core::Freezer::Id;
    using Field = core::Freezer::Field;

    [[nodiscard]] static constexpr std::string_view entity_name() {
      return "freezer";
    }

    [[nodiscard]] static constexpr Field tombstone_field() {
      return Field::ArchivedAt;
    }
  };

  template <> struct EntityTraits<core::StorageContainer> {
    using Id = core::StorageContainer::Id;
    using Field = core::StorageContainer::Field;

    [[nodiscard]] static constexpr std::string_view entity_name() {
      return "storage_container";
    }

    [[nodiscard]] static constexpr Field tombstone_field() {
      return Field::ArchivedAt;
    }
  };

} // namespace fmgr::storage

#endif // FMGR_STORAGE_FREEZERTRAITS_H
