// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_STORAGE_SHAREREQUESTTRAITS_H
#define FMGR_STORAGE_SHAREREQUESTTRAITS_H

#include "core/share_request.h"
#include "storage/IStorageBackend.h"

#include <string_view>

namespace fmgr::storage {

  template <> struct EntityTraits<core::ShareRequest> {
    using Id = core::ShareRequest::Id;
    using Field = core::ShareRequest::Field;

    [[nodiscard]] static constexpr std::string_view entity_name() {
      return "share_request";
    }

    // soft_delete() sets status = revoked; not a traditional archived_at_micros tombstone.
    // This field is documentation only; the repository hand-writes the status filter.
    [[nodiscard]] static constexpr Field tombstone_field() {
      return Field::Status;
    }
  };

  template <> struct EntityTraits<core::ShareRequestApproval> {
    using Id = core::ShareRequestApprovalId;
    using Field = core::ShareRequestApproval::Field;

    [[nodiscard]] static constexpr std::string_view entity_name() {
      return "share_request_approval";
    }

    // Append-only; no tombstone column. Dummy field required by the EntityTraits contract.
    [[nodiscard]] static constexpr Field tombstone_field() {
      return Field::ShareRequestId;
    }
  };

} // namespace fmgr::storage

#endif // FMGR_STORAGE_SHAREREQUESTTRAITS_H
