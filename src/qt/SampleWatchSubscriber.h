// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_QT_SAMPLEWATCHSUBSCRIBER_H
#define FMGR_QT_SAMPLEWATCHSUBSCRIBER_H

#include <atomic>
#include <functional>
#include <memory>
#include <thread>

#include <QObject>
#include <QString>

#include <grpcpp/client_context.h>

#include "fmgr/v1/sample.grpc.pb.h"
#include "qt/SampleServiceClient.h"  // SampleFilter

namespace fmgr::qt {

// Pump a WatchSampleList server-stream into `sink`, one call per streamed
// sample, until the stream ends (server done, or `ctx` cancelled from another
// thread via TryCancel). Sets the bearer on `ctx` before opening the reader, so
// `ctx` must be fresh and must outlive this call. Returns the final gRPC status.
//
// Pure and synchronous: no QObject, no thread, no event loop — so it is unit
// testable against a real in-process server with a plain std::vector sink.
grpc::Status driveSampleWatch(
    v1::SampleService::StubInterface& stub,
    const v1::WatchSampleListRequest& req, const QString& token,
    grpc::ClientContext& ctx,
    const std::function<void(const v1::Sample&)>& sink);

// Subscribes to the live WatchSampleList feed for a scope and emits changed()
// on the owning (GUI) thread each time a sample arrives. The blocking gRPC read
// loop runs on a dedicated worker thread; each frame is marshalled back to the
// owning thread via a queued invocation, the same threading contract as the
// REST SseBridge. changed() is a coalescing ping — the consumer debounces it
// and reloads the current scope (a full per-row delta is a later refinement).
class SampleWatchSubscriber : public QObject {
  Q_OBJECT

 public:
  explicit SampleWatchSubscriber(
      std::unique_ptr<v1::SampleService::StubInterface> stub,
      QObject* parent = nullptr);
  ~SampleWatchSubscriber() override;

  SampleWatchSubscriber(const SampleWatchSubscriber&) = delete;
  SampleWatchSubscriber& operator=(const SampleWatchSubscriber&) = delete;
  SampleWatchSubscriber(SampleWatchSubscriber&&) = delete;
  SampleWatchSubscriber& operator=(SampleWatchSubscriber&&) = delete;

  void setToken(const QString& session_token) { token_ = session_token; }

  // Stop any running subscription, then start a fresh one scoped to `filter`
  // (lab_id required; box_id/item_type_id narrow it). The feed starts from
  // "now", so only changes after this call stream.
  void start(const SampleFilter& filter);

  // Cancel the open stream and join the worker thread. Idempotent and safe to
  // call from the destructor; the worker is always joined before the stub is
  // freed.
  void stop();

 signals:
  void changed();

 private:
  std::unique_ptr<v1::SampleService::StubInterface> stub_;
  QString token_;
  std::unique_ptr<grpc::ClientContext> ctx_;
  std::thread worker_;
};

}  // namespace fmgr::qt

#endif  // FMGR_QT_SAMPLEWATCHSUBSCRIBER_H
