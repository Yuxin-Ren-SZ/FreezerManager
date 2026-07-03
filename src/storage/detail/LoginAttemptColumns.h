// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_STORAGE_DETAIL_LOGINATTEMPTCOLUMNS_H
#define FMGR_STORAGE_DETAIL_LOGINATTEMPTCOLUMNS_H

#include "core/login_attempt.h"
#include "storage/IStorageBackend.h"

#include <string>

// Backend-neutral column-name mapping and validation for the login_attempt
// (lockout) entity. Column names are identical across both schemas.
namespace fmgr::storage::detail {

  [[nodiscard]] inline std::string login_attempt_column_name(core::LoginAttempt::Field field) {
    switch (field) {
    case core::LoginAttempt::Field::Id:
      return "id";
    case core::LoginAttempt::Field::Email:
      return "email";
    case core::LoginAttempt::Field::FailureCount:
      return "failure_count";
    case core::LoginAttempt::Field::LockedUntil:
      return "locked_until_micros";
    case core::LoginAttempt::Field::LastActivity:
      return "last_activity_micros";
    case core::LoginAttempt::Field::ClearedAt:
      return "cleared_at_micros";
    }
    throw ConstraintViolation("unknown login_attempt field");
  }

  inline void validate_login_attempt(const core::LoginAttempt& attempt) {
    if (attempt.email.empty()) {
      throw ConstraintViolation("login_attempt email is required");
    }
  }

} // namespace fmgr::storage::detail

#endif // FMGR_STORAGE_DETAIL_LOGINATTEMPTCOLUMNS_H
