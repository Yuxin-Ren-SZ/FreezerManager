// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_STORAGE_ITEMTYPETRAITS_H
#define FMGR_STORAGE_ITEMTYPETRAITS_H

#include "core/item_type.h"
#include "storage/IStorageBackend.h"

#include <string_view>

namespace fmgr::storage {

  template <> struct EntityTraits<core::ItemType> {
    using Id = core::ItemType::Id;
    using Field = core::ItemType::Field;

    [[nodiscard]] static constexpr std::string_view entity_name() {
      return "item_type";
    }

    [[nodiscard]] static constexpr Field tombstone_field() {
      return Field::ArchivedAt;
    }
  };

  template <> struct EntityTraits<core::CustomFieldDefinition> {
    using Id = core::CustomFieldDefinition::Id;
    using Field = core::CustomFieldDefinition::Field;

    [[nodiscard]] static constexpr std::string_view entity_name() {
      return "custom_field_definition";
    }

    [[nodiscard]] static constexpr Field tombstone_field() {
      return Field::ArchivedAt;
    }
  };

} // namespace fmgr::storage

#endif // FMGR_STORAGE_ITEMTYPETRAITS_H
