// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_STORAGE_MFA_SECURITY_STATE_TRAITS_H
#define FMGR_STORAGE_MFA_SECURITY_STATE_TRAITS_H

#include "core/mfa_security_state.h"
#include "storage/IStorageBackend.h"

namespace fmgr::storage {
  template <> struct EntityTraits<core::MfaSecurityState> {
    using Id = core::UserId;
    using Field = core::MfaSecurityState::Field;
    [[nodiscard]] static constexpr std::string_view entity_name() {
      return "mfa_security_state";
    }
  };
} // namespace fmgr::storage
#endif
