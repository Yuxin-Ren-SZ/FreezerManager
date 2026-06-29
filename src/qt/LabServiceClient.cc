// SPDX-License-Identifier: AGPL-3.0-or-later

#include "qt/LabServiceClient.h"

#include <utility>

#include <grpcpp/client_context.h>

namespace fmgr::qt {
namespace {

// Bearer tokens travel as HTTP/2 metadata: "authorization: Bearer <token>"
// (see proto/fmgr/v1/auth.proto). Same scheme as AuthServiceClient.
void setBearer(grpc::ClientContext* ctx, const QString& token) {
  ctx->AddMetadata("authorization", "Bearer " + token.toStdString());
}

LabServiceClient::LabRow toRow(const v1::Lab& lab) {
  LabServiceClient::LabRow row;
  row.id = QString::fromStdString(lab.id());
  row.name = QString::fromStdString(lab.name());
  row.contact = QString::fromStdString(lab.contact());
  row.is_phi_enabled = lab.is_phi_enabled();
  return row;
}

}  // namespace

LabServiceClient::LabServiceClient(
    std::unique_ptr<v1::LabService::StubInterface> stub)
    : stub_(std::move(stub)) {}

LabServiceClient::ListLabsResult LabServiceClient::listLabs(
    const QString& session_token) {
  v1::ListLabsRequest req;

  grpc::ClientContext ctx;
  setBearer(&ctx, session_token);
  v1::ListLabsResponse resp;
  const grpc::Status status = stub_->ListLabs(&ctx, req, &resp);

  ListLabsResult result;
  if (!status.ok()) {
    result.error = status.error_message();
    return result;
  }
  result.ok = true;
  result.labs.reserve(resp.labs_size());
  for (const v1::Lab& lab : resp.labs()) {
    result.labs.push_back(toRow(lab));
  }
  return result;
}

LabServiceClient::GetLabResult LabServiceClient::getLab(
    const QString& session_token, const QString& lab_id) {
  v1::GetLabRequest req;
  req.set_lab_id(lab_id.toStdString());

  grpc::ClientContext ctx;
  setBearer(&ctx, session_token);
  v1::GetLabResponse resp;
  const grpc::Status status = stub_->GetLab(&ctx, req, &resp);

  GetLabResult result;
  if (!status.ok()) {
    result.error = status.error_message();
    return result;
  }
  result.ok = true;
  result.lab = toRow(resp.lab());
  return result;
}

}  // namespace fmgr::qt
