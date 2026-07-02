// SPDX-License-Identifier: AGPL-3.0-or-later

#include "qt/SampleWatchSubscriber.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <gtest/gtest.h>

#include "fmgr/v1/sample.grpc.pb.h"
#include "qt/SampleServiceClient.h"

namespace {

  using fmgr::qt::driveSampleWatch;
  using fmgr::qt::SampleFilter;
  using fmgr::qt::SampleWatchSubscriber;

  // In-process fake SampleService whose WatchSampleList either streams a fixed
  // number of scripted samples and completes, or (block_after >= 0) streams that
  // many then parks until the client cancels — exercising the TryCancel path.
  class FakeWatchService final : public fmgr::v1::SampleService::Service {
  public:
    int stream_count = 3; // samples to emit before completing
    int block_after = -1; // if >= 0, emit this many then block until cancelled

    // Captured at stream-open.
    std::string seen_lab_id;
    bool seen_has_box = false;
    std::string seen_box_id;
    bool seen_has_item_type = false;
    std::string seen_item_type_id;
    bool seen_has_since = false;
    std::string seen_authorization;
    std::atomic<int> emitted{0};

    grpc::Status WatchSampleList(grpc::ServerContext* ctx,
                                 const fmgr::v1::WatchSampleListRequest* req,
                                 grpc::ServerWriter<fmgr::v1::Sample>* writer) override {
      seen_lab_id = req->lab_id();
      seen_has_box = req->has_box_id();
      seen_box_id = req->box_id();
      seen_has_item_type = req->has_item_type_id();
      seen_item_type_id = req->item_type_id();
      seen_has_since = req->has_since();
      seen_authorization = metadata(ctx, "authorization");

      const int initial = block_after >= 0 ? block_after : stream_count;
      for (int i = 0; i < initial; ++i) {
        fmgr::v1::Sample sample;
        sample.set_id("s-" + std::to_string(i));
        sample.set_name("Sample " + std::to_string(i));
        if (!writer->Write(sample)) {
          return grpc::Status::OK;
        }
        emitted.fetch_add(1);
      }
      if (block_after >= 0) {
        while (!ctx->IsCancelled()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
      }
      return grpc::Status::OK;
    }

  private:
    static std::string metadata(grpc::ServerContext* ctx, const char* key) {
      const auto& md = ctx->client_metadata();
      auto it = md.find(key);
      if (it == md.end()) {
        return {};
      }
      return std::string(it->second.data(), it->second.size());
    }
  };

  class SampleWatchSubscriberTest : public ::testing::Test {
  protected:
    void SetUp() override {
      grpc::ServerBuilder builder;
      builder.RegisterService(&service_);
      server_ = builder.BuildAndStart();
      ASSERT_NE(server_, nullptr);
      channel_ = server_->InProcessChannel(grpc::ChannelArguments());
    }

    void TearDown() override {
      if (server_) {
        server_->Shutdown();
        server_->Wait();
      }
    }

    [[nodiscard]] std::unique_ptr<fmgr::v1::SampleService::StubInterface> stub() {
      return fmgr::v1::SampleService::NewStub(channel_);
    }

    FakeWatchService service_;
    std::unique_ptr<grpc::Server> server_;
    std::shared_ptr<grpc::Channel> channel_;
  };

  // drive() pumps every streamed sample into the sink, in order, then reports OK.
  TEST_F(SampleWatchSubscriberTest, DrivePumpsAllSamplesInOrder) {
    service_.stream_count = 3;
    auto stub = this->stub();

    fmgr::v1::WatchSampleListRequest req;
    req.set_lab_id("lab-1");
    req.set_box_id("box-1");
    req.set_item_type_id("it-1");

    std::vector<std::string> ids;
    grpc::ClientContext ctx;
    const grpc::Status status =
        driveSampleWatch(*stub, req, QStringLiteral("tok-1"), ctx,
                         [&ids](const fmgr::v1::Sample& s) { ids.push_back(s.id()); });

    EXPECT_TRUE(status.ok()) << status.error_message();
    ASSERT_EQ(ids.size(), 3U);
    EXPECT_EQ(ids[0], "s-0");
    EXPECT_EQ(ids[1], "s-1");
    EXPECT_EQ(ids[2], "s-2");

    // Request and bearer reached the server intact.
    EXPECT_EQ(service_.seen_lab_id, "lab-1");
    EXPECT_TRUE(service_.seen_has_box);
    EXPECT_EQ(service_.seen_box_id, "box-1");
    EXPECT_TRUE(service_.seen_has_item_type);
    EXPECT_EQ(service_.seen_item_type_id, "it-1");
    EXPECT_EQ(service_.seen_authorization, "Bearer tok-1");
  }

  // A blocked stream is unblocked by cancelling the context from another thread —
  // the same mechanism SampleWatchSubscriber::stop() uses.
  TEST_F(SampleWatchSubscriberTest, TryCancelEndsABlockedStream) {
    service_.block_after = 1; // emit one, then park until cancelled
    auto stub = this->stub();

    fmgr::v1::WatchSampleListRequest req;
    req.set_lab_id("lab-1");

    std::atomic<int> received{0};
    grpc::ClientContext ctx;
    std::thread worker([&] {
      driveSampleWatch(*stub, req, QStringLiteral("tok-1"), ctx,
                       [&received](const fmgr::v1::Sample&) { received.fetch_add(1); });
    });

    // Wait for the first sample, then cancel; the worker must then return.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (received.load() < 1 && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_EQ(received.load(), 1);
    ctx.TryCancel();
    worker.join(); // would hang forever if TryCancel did not unblock Read
    SUCCEED();
  }

  // start()/stop() lifecycle: starting against a parked stream and then stopping
  // must join cleanly; a second stop() is a no-op. (changed() is not asserted —
  // queued delivery needs a running QApplication event loop, which these headless
  // tests do not run; the sink behaviour is covered by drive() above.)
  TEST_F(SampleWatchSubscriberTest, StartThenStopIsSafeAndIdempotent) {
    service_.block_after = 1;
    SampleWatchSubscriber subscriber(this->stub());
    subscriber.setToken(QStringLiteral("tok-1"));

    SampleFilter filter;
    filter.lab_id = QStringLiteral("lab-1");
    subscriber.start(filter);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (service_.emitted.load() < 1 && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_GE(service_.emitted.load(), 1);

    subscriber.stop();
    subscriber.stop(); // idempotent
    SUCCEED();
  }

  // start() while already running stops the previous subscription first (no leak,
  // no hang), then runs the new scope.
  TEST_F(SampleWatchSubscriberTest, RestartStopsPreviousSubscription) {
    service_.block_after = 1;
    SampleWatchSubscriber subscriber(this->stub());
    subscriber.setToken(QStringLiteral("tok-1"));

    SampleFilter filter;
    filter.lab_id = QStringLiteral("lab-1");
    subscriber.start(filter);
    subscriber.start(filter); // restart: must not deadlock joining itself
    subscriber.stop();
    SUCCEED();
  }

} // namespace
