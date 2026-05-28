// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_STORAGE_BOXGEOMETRYTRAITS_H
#define FMGR_STORAGE_BOXGEOMETRYTRAITS_H

#include "core/box.h"
#include "storage/IStorageBackend.h"

#include <string_view>

namespace fmgr::storage {

  template <> struct EntityTraits<core::ContainerType> {
    using Id = core::ContainerType::Id;
    using Field = core::ContainerType::Field;

    [[nodiscard]] static constexpr std::string_view entity_name() {
      return "container_type";
    }

    [[nodiscard]] static constexpr Field tombstone_field() {
      return Field::ArchivedAt;
    }
  };

  template <> struct EntityTraits<core::BoxType> {
    using Id = core::BoxType::Id;
    using Field = core::BoxType::Field;

    [[nodiscard]] static constexpr std::string_view entity_name() {
      return "box_type";
    }

    [[nodiscard]] static constexpr Field tombstone_field() {
      return Field::ArchivedAt;
    }
  };

  template <> struct EntityTraits<core::Box> {
    using Id = core::Box::Id;
    using Field = core::Box::Field;

    [[nodiscard]] static constexpr std::string_view entity_name() {
      return "box";
    }

    [[nodiscard]] static constexpr Field tombstone_field() {
      return Field::ArchivedAt;
    }
  };

} // namespace fmgr::storage

#endif // FMGR_STORAGE_BOXGEOMETRYTRAITS_H
