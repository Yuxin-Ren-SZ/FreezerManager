// SPDX-License-Identifier: AGPL-3.0-or-later

// E1: Auth provider value types shared across all IAuthProvider implementations.
//
// AuthCredentials: credential variant passed to IAuthProvider::authenticate().
// ClientInfo:      per-request metadata (IP, user-agent) for session records.
// AuthToken:       result of a successful primary-factor authentication,
//                  containing the plaintext bearer token shown exactly once.
// SessionContext:  per-request context resolved from a valid bearer token;
//                  carries the caller's effective permission set and the set
//                  of labs whose data they may query.
//
// Error hierarchy:
//   AuthError          (base; is-a std::runtime_error)
//   ├─ InvalidCredentials   wrong password / unknown user / bad token format
//   ├─ AccountLocked        too many failures; includes locked_until timestamp
//   ├─ MfaRequired          primary factor OK; TOTP still pending
//   ├─ TokenExpired         session or API token past expiry
//   ├─ TokenRevoked         session or API token was explicitly revoked
//   └─ PermissionDenied     valid session; required permission not held
#ifndef FMGR_AUTH_AUTHTYPES_H
#define FMGR_AUTH_AUTHTYPES_H

#include "core/ids.h"
#include "core/permissions.h"
#include "core/timestamp.h"

#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <variant>

namespace fmgr::auth {

  // ---- Credential variants ----

  // Username + password for the LocalAuthProvider.
  struct PasswordCredentials {
    std::string email;
    std::string password; // plaintext; never stored or logged
  };

  // Full plaintext API token for the ApiToken flow.
  struct ApiTokenCredentials {
    std::string token; // "fmgr_pat_<prefix>_<secret>" full token string
  };

  using AuthCredentials = std::variant<PasswordCredentials, ApiTokenCredentials>;

  // ---- Per-request client metadata ----

  struct ClientInfo {
    std::optional<std::string> ip;
    std::optional<std::string> user_agent;
  };

  // ---- Results ----

  // Returned after a successful primary-factor authentication.
  // mfa_complete == false when the user's role requires TOTP:
  //   the caller must call IAuthProvider::verify_totp() before the session
  //   gains full access; RPC middleware rejects non-MFA RPCs until then.
  struct AuthToken {
    core::SessionId session_id;
    std::string plaintext_token; // shown once; must NOT be logged or stored
    bool mfa_complete{true};
  };

  // Resolved per-request context derived from a valid bearer token.
  // Permission grants may be cached by auth providers between requests.
  //
  // permissions_by_lab = active lab memberships and their lab-scoped grants.
  // global_permissions = deployment-wide grants that are not tied to one lab.
  struct SessionContext {
    core::SessionId session_id;
    core::UserId user_id;
    std::map<core::LabId, std::set<core::Permission>> permissions_by_lab;
    std::set<core::Permission> global_permissions;
    bool mfa_complete{true};

    [[nodiscard]] bool can_see_lab(const core::LabId& lab_id) const {
      return permissions_by_lab.contains(lab_id);
    }

    [[nodiscard]] bool has_for_lab(const core::LabId& lab_id, core::Permission perm) const {
      const auto iter = permissions_by_lab.find(lab_id);
      return iter != permissions_by_lab.end() && iter->second.contains(perm);
    }

    [[nodiscard]] bool has_global(core::Permission perm) const {
      return global_permissions.contains(perm);
    }
  };

  // ---- Error hierarchy ----

  // Base for all authentication and authorisation errors.
  struct AuthError : std::runtime_error {
    using std::runtime_error::runtime_error;
  };

  // Wrong password, unknown user, or invalid token format.
  struct InvalidCredentials : AuthError {
    using AuthError::AuthError;
  };

  // Too many failed attempts — account is temporarily locked.
  struct AccountLocked : AuthError {
    core::Timestamp locked_until;

    explicit AccountLocked(core::Timestamp when)
        : AuthError("account locked until " + std::to_string(when.unix_micros())),
          locked_until(when) {}
  };

  // Primary credentials accepted but TOTP verification is still required.
  struct MfaRequired : AuthError {
    using AuthError::AuthError;
  };

  // Session or API token has passed its configured expiry time.
  struct TokenExpired : AuthError {
    using AuthError::AuthError;
  };

  // Session or API token has been explicitly revoked.
  struct TokenRevoked : AuthError {
    using AuthError::AuthError;
  };

  // Caller's session is valid but lacks the required permission for the RPC.
  struct PermissionDenied : AuthError {
    using AuthError::AuthError;
  };

} // namespace fmgr::auth

#endif // FMGR_AUTH_AUTHTYPES_H
