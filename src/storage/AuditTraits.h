// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_STORAGE_AUDITTRAITS_H
#define FMGR_STORAGE_AUDITTRAITS_H

#include "core/audit_event.h"
#include "storage/IStorageBackend.h"

#include <string_view>

namespace fmgr::storage {

  template <> struct EntityTraits<core::AuditEvent> {
    using Id = core::AuditEvent::Id;
    using Field = core::AuditEvent::Field;

    [[nodiscard]] static constexpr std::string_view entity_name() {
      return "audit_event";
    }

    // AuditEvent is append-only: soft_delete() throws UnsupportedOperation.
    // This dummy tombstone field is never accessed at runtime.
    [[nodiscard]] static constexpr Field tombstone_field() {
      return Field::Id;
    }
  };

} // namespace fmgr::storage

#endif // FMGR_STORAGE_AUDITTRAITS_H
