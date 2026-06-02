// SPDX-License-Identifier: AGPL-3.0-or-later

// E1: Abstract authentication provider interface.
//
// Implementations (all must satisfy the same SessionContext semantics):
//   LocalAuthProvider  (E2)  — Argon2id + TOTP
//   OidcAuthProvider   (E6)  — OIDC/PKCE; per-lab issuer config
//   LdapAuthProvider   (E6)  — bind + search; group → role mapping
//   MtlsAuthProvider   (E6)  — client certs for machine/instrument clients
//
// Thread-safety: all methods MUST be safe to call concurrently from multiple
// request-handler threads.  Implementations must not rely on per-call mutable
// state in the provider object itself.
#ifndef FMGR_AUTH_IAUTHPROVIDER_H
#define FMGR_AUTH_IAUTHPROVIDER_H

#include "auth/AuthTypes.h"
#include "storage/IStorageBackend.h"

#include <string_view>

namespace fmgr::auth {

  class IAuthProvider {
  public:
    virtual ~IAuthProvider() = default;

    // Verify credentials and create a new Session row.
    // Returns the plaintext bearer token (displayed to the user exactly once;
    // never logged or re-read from storage).
    // Throws: InvalidCredentials, AccountLocked, AuthError subclasses.
    virtual AuthToken authenticate(const AuthCredentials& creds, const ClientInfo& client) = 0;

    // Resolve a bearer token to a per-request SessionContext.
    // This call is read-only and must not open a write transaction.
    // Throws: TokenExpired, TokenRevoked, InvalidCredentials.
    virtual SessionContext validate_token(std::string_view bearer_token) = 0;

    // Verify a TOTP code for the given session.
    // On success, sets the session's mfa_complete flag in storage.
    // Throws: InvalidCredentials (wrong code or session not awaiting MFA).
    virtual void verify_totp(const core::SessionId& session_id, std::string_view totp_code) = 0;

    // Revoke a single session by ID.
    // Idempotent: revoking an already-revoked session is a no-op.
    // The mutation is audited using ctx.
    virtual void revoke_session(const core::SessionId& session_id,
                                const storage::MutationContext& ctx) = 0;

    // Revoke all active sessions belonging to a user ("log out everywhere").
    // The mutation is audited using ctx.
    virtual void revoke_all_sessions(const core::UserId& uid,
                                     const storage::MutationContext& ctx) = 0;
  };

} // namespace fmgr::auth

#endif // FMGR_AUTH_IAUTHPROVIDER_H
