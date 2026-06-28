// SPDX-License-Identifier: AGPL-3.0-or-later

#include "qt/BoxServiceClient.h"

#include <utility>

#include <grpcpp/client_context.h>

namespace fmgr::qt {
namespace {

void setBearer(grpc::ClientContext* ctx, const QString& token) {
  ctx->AddMetadata("authorization", "Bearer " + token.toStdString());
}

BoxServiceClient::FreezerRow toRow(const v1::Freezer& freezer) {
  BoxServiceClient::FreezerRow row;
  row.id = QString::fromStdString(freezer.id());
  row.lab_id = QString::fromStdString(freezer.lab_id());
  row.name = QString::fromStdString(freezer.name());
  row.location = QString::fromStdString(freezer.location());
  row.model = QString::fromStdString(freezer.model());
  row.layout_root_id = QString::fromStdString(freezer.layout_root_id());
  return row;
}

BoxServiceClient::ContainerRow toRow(const v1::StorageContainer& container) {
  BoxServiceClient::ContainerRow row;
  row.id = QString::fromStdString(container.id());
  row.lab_id = QString::fromStdString(container.lab_id());
  if (container.has_parent_id()) {
    row.parent_id = QString::fromStdString(container.parent_id());
  }
  row.kind = container.kind();
  row.name = QString::fromStdString(container.name());
  row.label = QString::fromStdString(container.label());
  return row;
}

}  // namespace

BoxServiceClient::BoxServiceClient(
    std::unique_ptr<v1::BoxService::StubInterface> stub)
    : stub_(std::move(stub)) {}

BoxServiceClient::ListFreezersResult BoxServiceClient::listFreezers(
    const QString& session_token, const QString& lab_id) {
  v1::ListFreezersRequest req;
  req.set_lab_id(lab_id.toStdString());

  grpc::ClientContext ctx;
  setBearer(&ctx, session_token);
  v1::ListFreezersResponse resp;
  const grpc::Status status = stub_->ListFreezers(&ctx, req, &resp);

  ListFreezersResult result;
  if (!status.ok()) {
    result.error = status.error_message();
    return result;
  }
  result.ok = true;
  result.freezers.reserve(resp.freezers_size());
  for (const v1::Freezer& freezer : resp.freezers()) {
    result.freezers.push_back(toRow(freezer));
  }
  return result;
}

BoxServiceClient::ListContainersResult BoxServiceClient::listStorageContainers(
    const QString& session_token, const QString& lab_id,
    const std::optional<QString>& parent_id) {
  v1::ListStorageContainersRequest req;
  req.set_lab_id(lab_id.toStdString());
  if (parent_id.has_value()) {
    req.set_parent_id(parent_id->toStdString());
  }

  grpc::ClientContext ctx;
  setBearer(&ctx, session_token);
  v1::ListStorageContainersResponse resp;
  const grpc::Status status = stub_->ListStorageContainers(&ctx, req, &resp);

  ListContainersResult result;
  if (!status.ok()) {
    result.error = status.error_message();
    return result;
  }
  result.ok = true;
  result.containers.reserve(resp.containers_size());
  for (const v1::StorageContainer& container : resp.containers()) {
    result.containers.push_back(toRow(container));
  }
  return result;
}

}  // namespace fmgr::qt
