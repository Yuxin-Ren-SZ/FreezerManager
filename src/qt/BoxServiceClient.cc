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

BoxServiceClient::BoxRow toRow(const v1::Box& box) {
  BoxServiceClient::BoxRow row;
  row.id = QString::fromStdString(box.id());
  row.lab_id = QString::fromStdString(box.lab_id());
  row.box_type_id = QString::fromStdString(box.box_type_id());
  row.storage_container_id = QString::fromStdString(box.storage_container_id());
  row.label = QString::fromStdString(box.label());
  return row;
}

BoxServiceClient::BoxTypeRow toRow(const v1::BoxType& box_type) {
  BoxServiceClient::BoxTypeRow row;
  row.id = QString::fromStdString(box_type.id());
  row.name = QString::fromStdString(box_type.name());
  row.positions.reserve(box_type.positions_size());
  for (const v1::BoxPosition& pos : box_type.positions()) {
    BoxServiceClient::PositionRow p;
    p.label = QString::fromStdString(pos.label());
    p.row = pos.row();
    p.col = pos.col();
    if (pos.has_z()) {
      p.z = pos.z();
    }
    p.accepts.reserve(pos.accepts_size());
    for (const std::string& token : pos.accepts()) {
      p.accepts.push_back(QString::fromStdString(token));
    }
    row.positions.push_back(std::move(p));
  }
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

BoxServiceClient::ListBoxesResult BoxServiceClient::listBoxes(
    const QString& session_token, const QString& lab_id,
    const QString& storage_container_id) {
  v1::ListBoxesRequest req;
  req.set_lab_id(lab_id.toStdString());
  req.set_storage_container_id(storage_container_id.toStdString());

  grpc::ClientContext ctx;
  setBearer(&ctx, session_token);
  v1::ListBoxesResponse resp;
  const grpc::Status status = stub_->ListBoxes(&ctx, req, &resp);

  ListBoxesResult result;
  if (!status.ok()) {
    result.error = status.error_message();
    return result;
  }
  result.ok = true;
  result.boxes.reserve(resp.boxes_size());
  for (const v1::Box& box : resp.boxes()) {
    result.boxes.push_back(toRow(box));
  }
  return result;
}

BoxServiceClient::GetBoxResult BoxServiceClient::getBox(
    const QString& session_token, const QString& box_id) {
  v1::GetBoxRequest req;
  req.set_box_id(box_id.toStdString());

  grpc::ClientContext ctx;
  setBearer(&ctx, session_token);
  v1::GetBoxResponse resp;
  const grpc::Status status = stub_->GetBox(&ctx, req, &resp);

  GetBoxResult result;
  if (!status.ok()) {
    result.error = status.error_message();
    return result;
  }
  result.ok = true;
  result.box = toRow(resp.box());
  return result;
}

BoxServiceClient::ListBoxTypesResult BoxServiceClient::listBoxTypes(
    const QString& session_token, const QString& lab_id) {
  v1::ListBoxTypesRequest req;
  req.set_lab_id(lab_id.toStdString());

  grpc::ClientContext ctx;
  setBearer(&ctx, session_token);
  v1::ListBoxTypesResponse resp;
  const grpc::Status status = stub_->ListBoxTypes(&ctx, req, &resp);

  ListBoxTypesResult result;
  if (!status.ok()) {
    result.error = status.error_message();
    return result;
  }
  result.ok = true;
  result.box_types.reserve(resp.box_types_size());
  for (const v1::BoxType& box_type : resp.box_types()) {
    result.box_types.push_back(toRow(box_type));
  }
  return result;
}

}  // namespace fmgr::qt
