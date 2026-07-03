// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_STORAGE_LOGINATTEMPTTRAITS_H
#define FMGR_STORAGE_LOGINATTEMPTTRAITS_H

#include "core/login_attempt.h"
#include "storage/IStorageBackend.h"

#include <string_view>

namespace fmgr::storage {

  template <> struct EntityTraits<core::LoginAttempt> {
    using Id = core::LoginAttempt::Id;
    using Field = core::LoginAttempt::Field;

    [[nodiscard]] static constexpr std::string_view entity_name() {
      return "login_attempt";
    }

    // soft_delete() sets cleared_at_micros = now(); a successful login clears the
    // active lockout counter for that email.
    [[nodiscard]] static constexpr Field tombstone_field() {
      return Field::ClearedAt;
    }
  };

} // namespace fmgr::storage

#endif // FMGR_STORAGE_LOGINATTEMPTTRAITS_H
