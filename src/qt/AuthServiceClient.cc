// SPDX-License-Identifier: AGPL-3.0-or-later

#include "qt/AuthServiceClient.h"

#include <utility>

#include <grpcpp/client_context.h>

namespace fmgr::qt {
namespace {

// Bearer tokens travel as HTTP/2 metadata: "authorization: Bearer <token>"
// (see proto/fmgr/v1/auth.proto).
void setBearer(grpc::ClientContext* ctx, const QString& token) {
  ctx->AddMetadata("authorization", "Bearer " + token.toStdString());
}

}  // namespace

AuthServiceClient::AuthServiceClient(
    std::unique_ptr<v1::AuthService::StubInterface> stub)
    : stub_(std::move(stub)) {}

AuthServiceClient::LoginResult AuthServiceClient::login(
    const QString& email, const QString& password) {
  v1::LoginRequest req;
  req.set_email(email.toStdString());
  req.set_password(password.toStdString());

  grpc::ClientContext ctx;
  v1::LoginResponse resp;
  const grpc::Status status = stub_->Login(&ctx, req, &resp);

  LoginResult result;
  if (!status.ok()) {
    result.error = status.error_message();
    return result;
  }
  result.ok = true;
  result.session_token = QString::fromStdString(resp.session_token());
  result.session_id = QString::fromStdString(resp.session_id());
  result.user_id = QString::fromStdString(resp.user_id());
  result.mfa_required = resp.mfa_required();
  return result;
}

AuthServiceClient::Result AuthServiceClient::submitMfa(
    const QString& session_token, const QString& totp_code) {
  v1::SubmitMfaRequest req;
  req.set_totp_code(totp_code.toStdString());

  grpc::ClientContext ctx;
  setBearer(&ctx, session_token);
  v1::SubmitMfaResponse resp;
  const grpc::Status status = stub_->SubmitMfa(&ctx, req, &resp);

  Result result;
  result.ok = status.ok();
  if (!status.ok()) {
    result.error = status.error_message();
  }
  return result;
}

AuthServiceClient::Result AuthServiceClient::logout(
    const QString& session_token) {
  v1::LogoutRequest req;

  grpc::ClientContext ctx;
  setBearer(&ctx, session_token);
  v1::LogoutResponse resp;
  const grpc::Status status = stub_->Logout(&ctx, req, &resp);

  Result result;
  result.ok = status.ok();
  if (!status.ok()) {
    result.error = status.error_message();
  }
  return result;
}

}  // namespace fmgr::qt
