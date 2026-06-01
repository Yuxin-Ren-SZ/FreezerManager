// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_STORAGE_SESSIONTRAITS_H
#define FMGR_STORAGE_SESSIONTRAITS_H

#include "core/session.h"
#include "storage/IStorageBackend.h"

#include <string_view>

namespace fmgr::storage {

  template <> struct EntityTraits<core::Session> {
    using Id = core::Session::Id;
    using Field = core::Session::Field;

    [[nodiscard]] static constexpr std::string_view entity_name() {
      return "session";
    }

    // soft_delete() sets revoked_at_micros = now().
    [[nodiscard]] static constexpr Field tombstone_field() {
      return Field::RevokedAt;
    }
  };

  template <> struct EntityTraits<core::ApiToken> {
    using Id = core::ApiToken::Id;
    using Field = core::ApiToken::Field;

    [[nodiscard]] static constexpr std::string_view entity_name() {
      return "api_token";
    }

    // soft_delete() sets revoked_at_micros = now().
    [[nodiscard]] static constexpr Field tombstone_field() {
      return Field::RevokedAt;
    }
  };

} // namespace fmgr::storage

#endif // FMGR_STORAGE_SESSIONTRAITS_H
