// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_STORAGE_DETAIL_ROLECOLUMNS_H
#define FMGR_STORAGE_DETAIL_ROLECOLUMNS_H

#include "core/role.h"
#include "storage/IStorageBackend.h"

// Backend-neutral pure validation for roles. The Role query path uses a fixed
// ordering rather than the generic Query DSL, so no column mapping is shared
// here — only the validator both backends must enforce identically.
namespace fmgr::storage::detail {

  inline void validate_role(const core::Role& role) {
    if (role.name.empty()) {
      throw ConstraintViolation("role name is required");
    }
    // Built-in roles are global (lab_id NULL); custom lab-defined roles carry a
    // lab_id. The DB schema enforces this through partial unique indexes; mirror
    // the contract here so the violation surfaces as a clean ConstraintViolation
    // rather than a UNIQUE error from a name collision against a seed-named role.
    if (role.is_builtin && role.lab_id.has_value()) {
      throw ConstraintViolation("built-in role must not be lab-scoped");
    }
  }

} // namespace fmgr::storage::detail

#endif // FMGR_STORAGE_DETAIL_ROLECOLUMNS_H
