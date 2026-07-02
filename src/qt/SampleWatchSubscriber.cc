// SPDX-License-Identifier: AGPL-3.0-or-later

#include "qt/SampleWatchSubscriber.h"

#include <chrono>
#include <cstdint>
#include <utility>

namespace fmgr::qt {

grpc::Status driveSampleWatch(
    v1::SampleService::StubInterface& stub,
    const v1::WatchSampleListRequest& req, const QString& token,
    grpc::ClientContext& ctx,
    const std::function<void(const v1::Sample&)>& sink) {
  ctx.AddMetadata("authorization", "Bearer " + token.toStdString());
  std::unique_ptr<grpc::ClientReaderInterface<v1::Sample>> reader =
      stub.WatchSampleList(&ctx, req);
  v1::Sample sample;
  while (reader->Read(&sample)) {
    sink(sample);
  }
  return reader->Finish();
}

SampleWatchSubscriber::SampleWatchSubscriber(
    std::unique_ptr<v1::SampleService::StubInterface> stub, QObject* parent)
    : QObject(parent), stub_(std::move(stub)) {}

SampleWatchSubscriber::~SampleWatchSubscriber() { stop(); }

void SampleWatchSubscriber::start(const SampleFilter& filter) {
  stop();

  v1::WatchSampleListRequest req;
  req.set_lab_id(filter.lab_id.toStdString());
  if (filter.box_id.has_value()) {
    req.set_box_id(filter.box_id->toStdString());
  }
  if (filter.item_type_id.has_value()) {
    req.set_item_type_id(filter.item_type_id->toStdString());
  }
  const auto now_micros =
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();
  req.mutable_since()->set_unix_micros(static_cast<std::int64_t>(now_micros));

  ctx_ = std::make_unique<grpc::ClientContext>();
  worker_ = std::thread([this, req] {
    driveSampleWatch(*stub_, req, token_, *ctx_, [this](const v1::Sample&) {
      // trantor/Qt event delivery: hop to the owning (GUI) thread. A bare
      // emit from the worker thread would be an unsafe cross-thread signal.
      QMetaObject::invokeMethod(
          this, [this] { emit changed(); }, ::Qt::QueuedConnection);
    });
  });
}

void SampleWatchSubscriber::stop() {
  if (worker_.joinable()) {
    if (ctx_) {
      ctx_->TryCancel();  // unblock a parked Read so the worker exits
    }
    worker_.join();
  }
  ctx_.reset();
}

}  // namespace fmgr::qt
