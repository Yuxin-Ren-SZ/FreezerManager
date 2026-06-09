// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_STORAGE_SQLITE_SQLITEAUTHZVERSION_H
#define FMGR_STORAGE_SQLITE_SQLITEAUTHZVERSION_H

#include "core/ids.h"
#include "storage/IStorageBackend.h"

#include <sqlite3.h>

#include <string>
#include <string_view>

// Shared helper for bumping users.authz_version inside an open transaction.
// Membership and role-permission repositories live in separate translation
// units, each with its own local Statement wrapper; this header uses the raw
// sqlite3 C API so both can call it without coupling to those wrappers.
namespace fmgr::storage::detail {

  // Runs a single-text-parameter UPDATE against `users`, throwing on driver
  // failure. Shared by the by-user and by-role bump helpers below.
  inline void run_authz_bump(sqlite3* handle, std::string_view sql, const std::string& id_str) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(handle, sql.data(), static_cast<int>(sql.size()), &stmt, nullptr) !=
        SQLITE_OK) {
      throw ConstraintViolation(std::string("bump authz_version: ") + sqlite3_errmsg(handle));
    }
    sqlite3_bind_text(stmt, 1, id_str.c_str(), static_cast<int>(id_str.size()), SQLITE_TRANSIENT);
    const int step_result = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (step_result != SQLITE_DONE) {
      throw ConstraintViolation(std::string("bump authz_version: ") + sqlite3_errmsg(handle));
    }
  }

  // Increments authz_version for a single user. No-op if the user row is absent
  // (e.g. a membership staged before its user in the same transaction — the
  // bump simply matches zero rows, which is harmless).
  inline void bump_authz_version_for_user(sqlite3* handle, const core::UserId& user_id) {
    run_authz_bump(handle, "UPDATE users SET authz_version = authz_version + 1 WHERE id = ?",
                   user_id.to_string());
  }

  // Increments authz_version for every user who currently holds the given role
  // (active membership). Used when a role's permission set changes.
  inline void bump_authz_version_for_role(sqlite3* handle, const core::RoleId& role_id) {
    run_authz_bump(handle,
                   "UPDATE users SET authz_version = authz_version + 1 WHERE id IN "
                   "(SELECT user_id FROM lab_memberships "
                   " WHERE role_id = ? AND revoked_at_micros IS NULL)",
                   role_id.to_string());
  }

} // namespace fmgr::storage::detail

#endif // FMGR_STORAGE_SQLITE_SQLITEAUTHZVERSION_H
