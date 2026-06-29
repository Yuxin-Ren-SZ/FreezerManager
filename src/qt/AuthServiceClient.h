// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_QT_AUTHSERVICECLIENT_H
#define FMGR_QT_AUTHSERVICECLIENT_H

#include <memory>
#include <string>

#include <QString>

#include "fmgr/v1/auth.grpc.pb.h"

namespace fmgr::qt {

// Synchronous, UI-agnostic wrapper over AuthService's authentication RPCs
// (Login, SubmitMfa, Logout). It owns a stub interface so it can be driven by a
// real channel in production or an in-process fake in tests.
//
// Results never throw: each call returns a struct whose ok flag and error string
// carry the gRPC status, so the dialog layer can display failures directly.
class AuthServiceClient {
 public:
  struct LoginResult {
    bool ok = false;
    std::string error;      // human-readable gRPC status message when !ok
    QString session_token;  // bearer for subsequent RPCs
    QString session_id;
    QString user_id;
    bool mfa_required = false;
  };

  struct Result {
    bool ok = false;
    std::string error;
  };

  explicit AuthServiceClient(
      std::unique_ptr<v1::AuthService::StubInterface> stub);

  // Authenticate with email + password (unauthenticated RPC).
  LoginResult login(const QString& email, const QString& password);

  // Complete MFA with a 6-digit TOTP code. Needs the bearer from login().
  Result submitMfa(const QString& session_token, const QString& totp_code);

  // Revoke the current session. Needs the bearer from login().
  Result logout(const QString& session_token);

 private:
  std::unique_ptr<v1::AuthService::StubInterface> stub_;
};

}  // namespace fmgr::qt

#endif  // FMGR_QT_AUTHSERVICECLIENT_H
