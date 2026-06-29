// SPDX-License-Identifier: AGPL-3.0-or-later

#include "qt/SampleServiceClient.h"

#include <utility>

#include <grpcpp/client_context.h>

namespace fmgr::qt {
namespace {

void setBearer(grpc::ClientContext* ctx, const QString& token) {
  ctx->AddMetadata("authorization", "Bearer " + token.toStdString());
}

SampleServiceClient::SampleRow toRow(const v1::Sample& sample) {
  SampleServiceClient::SampleRow row;
  row.id = QString::fromStdString(sample.id());
  row.name = QString::fromStdString(sample.name());
  if (sample.has_barcode()) {
    row.barcode = QString::fromStdString(sample.barcode());
  }
  row.status = sample.status();
  if (sample.has_box_id()) {
    row.box_id = QString::fromStdString(sample.box_id());
  }
  if (sample.has_position_label()) {
    row.position_label = QString::fromStdString(sample.position_label());
  }
  if (sample.has_volume_value()) {
    row.volume_value = sample.volume_value();
  }
  if (sample.has_volume_unit()) {
    row.volume_unit = QString::fromStdString(sample.volume_unit());
  }
  row.item_type_id = QString::fromStdString(sample.item_type_id());
  return row;
}

}  // namespace

SampleServiceClient::SampleServiceClient(
    std::unique_ptr<v1::SampleService::StubInterface> stub)
    : stub_(std::move(stub)) {}

SampleServiceClient::ListSamplesResult SampleServiceClient::listSamples(
    const QString& session_token, const SampleFilter& filter,
    const QString& page_token) {
  v1::ListSamplesRequest req;
  req.set_lab_id(filter.lab_id.toStdString());
  req.set_include_archived(filter.include_archived);
  if (filter.box_id.has_value()) {
    req.set_box_id(filter.box_id->toStdString());
  }
  if (filter.item_type_id.has_value()) {
    req.set_item_type_id(filter.item_type_id->toStdString());
  }
  if (filter.barcode.has_value()) {
    req.set_barcode(filter.barcode->toStdString());
  }
  if (filter.status.has_value()) {
    req.set_status(*filter.status);
  }
  if (!page_token.isEmpty()) {
    req.mutable_page()->set_page_token(page_token.toStdString());
  }

  grpc::ClientContext ctx;
  setBearer(&ctx, session_token);
  v1::ListSamplesResponse resp;
  const grpc::Status status = stub_->ListSamples(&ctx, req, &resp);

  ListSamplesResult result;
  if (!status.ok()) {
    result.error = status.error_message();
    return result;
  }
  result.ok = true;
  result.samples.reserve(resp.samples_size());
  for (const v1::Sample& sample : resp.samples()) {
    result.samples.push_back(toRow(sample));
  }
  result.next_page_token =
      QString::fromStdString(resp.page().next_page_token());
  return result;
}

SampleServiceClient::GetSampleResult SampleServiceClient::getSample(
    const QString& session_token, const QString& sample_id) {
  v1::GetSampleRequest req;
  req.set_sample_id(sample_id.toStdString());

  grpc::ClientContext ctx;
  setBearer(&ctx, session_token);
  v1::GetSampleResponse resp;
  const grpc::Status status = stub_->GetSample(&ctx, req, &resp);

  GetSampleResult result;
  if (!status.ok()) {
    result.error = status.error_message();
    return result;
  }
  result.ok = true;
  result.sample = toRow(resp.sample());
  return result;
}

SampleServiceClient::GetSampleResult SampleServiceClient::moveSample(
    const QString& session_token, const QString& sample_id,
    const QString& dest_box_id, const QString& dest_position) {
  v1::MoveSampleRequest req;
  req.set_sample_id(sample_id.toStdString());
  req.set_dest_box_id(dest_box_id.toStdString());
  req.set_dest_position(dest_position.toStdString());

  grpc::ClientContext ctx;
  setBearer(&ctx, session_token);
  v1::MoveSampleResponse resp;
  const grpc::Status status = stub_->MoveSample(&ctx, req, &resp);

  GetSampleResult result;
  if (!status.ok()) {
    result.error = status.error_message();
    return result;
  }
  result.ok = true;
  result.sample = toRow(resp.sample());
  return result;
}

SampleServiceClient::GetSampleResult SampleServiceClient::checkoutSample(
    const QString& session_token, const QString& sample_id,
    v1::CheckoutAction action, std::optional<double> volume_used,
    const QString& volume_unit, const QString& reason) {
  v1::CheckoutSampleRequest req;
  req.set_sample_id(sample_id.toStdString());
  req.set_action(action);
  if (volume_used.has_value()) {
    req.set_volume_used(*volume_used);
  }
  if (!volume_unit.isEmpty()) {
    req.set_volume_unit(volume_unit.toStdString());
  }
  if (!reason.isEmpty()) {
    req.set_reason(reason.toStdString());
  }

  grpc::ClientContext ctx;
  setBearer(&ctx, session_token);
  v1::CheckoutSampleResponse resp;
  const grpc::Status status = stub_->CheckoutSample(&ctx, req, &resp);

  GetSampleResult result;
  if (!status.ok()) {
    result.error = status.error_message();
    return result;
  }
  result.ok = true;
  result.sample = toRow(resp.sample());
  return result;
}

SampleServiceClient::ExportCsvResult SampleServiceClient::exportSamplesCsv(
    const QString& session_token, const QString& lab_id, bool include_archived) {
  v1::ExportSamplesCsvRequest req;
  req.set_lab_id(lab_id.toStdString());
  req.set_include_archived(include_archived);

  grpc::ClientContext ctx;
  setBearer(&ctx, session_token);
  v1::ExportSamplesCsvResponse resp;
  const grpc::Status status = stub_->ExportSamplesCsv(&ctx, req, &resp);

  ExportCsvResult result;
  if (!status.ok()) {
    result.error = status.error_message();
    return result;
  }
  result.ok = true;
  result.csv = QString::fromStdString(resp.csv_content());
  return result;
}

}  // namespace fmgr::qt
