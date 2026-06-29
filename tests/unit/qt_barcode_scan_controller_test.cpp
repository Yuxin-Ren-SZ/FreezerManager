// SPDX-License-Identifier: AGPL-3.0-or-later

#include "qt/BarcodeScanController.h"

#include <memory>
#include <string>

#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <gtest/gtest.h>

#include "fmgr/v1/sample.grpc.pb.h"
#include "qt/SampleServiceClient.h"

namespace {

using fmgr::qt::BarcodeScanController;
using fmgr::qt::SampleServiceClient;

// Fake that resolves barcode "BC-OK" to sample s-1 and records whether/how
// CheckoutSample was called.
class FakeSampleService final : public fmgr::v1::SampleService::Service {
 public:
  bool checkout_called = false;
  fmgr::v1::CheckoutAction seen_action = fmgr::v1::CHECKOUT_ACTION_UNSPECIFIED;
  std::string seen_checkout_id;
  bool fail_checkout = false;

  grpc::Status ListSamples(grpc::ServerContext* /*ctx*/,
                           const fmgr::v1::ListSamplesRequest* req,
                           fmgr::v1::ListSamplesResponse* resp) override {
    if (req->has_barcode() && req->barcode() == "BC-OK") {
      fmgr::v1::Sample* s = resp->add_samples();
      s->set_id("s-1");
      s->set_name("Sample 1");
      s->set_barcode("BC-OK");
    }
    return grpc::Status::OK;
  }

  grpc::Status CheckoutSample(grpc::ServerContext* /*ctx*/,
                              const fmgr::v1::CheckoutSampleRequest* req,
                              fmgr::v1::CheckoutSampleResponse* resp) override {
    checkout_called = true;
    seen_action = req->action();
    seen_checkout_id = req->sample_id();
    if (fail_checkout) {
      return {grpc::StatusCode::FAILED_PRECONDITION, "already checked out"};
    }
    resp->mutable_sample()->set_id(req->sample_id());
    return grpc::Status::OK;
  }
};

class BarcodeScanControllerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    grpc::ServerBuilder builder;
    builder.RegisterService(&service_);
    server_ = builder.BuildAndStart();
    ASSERT_NE(server_, nullptr);
    auto channel = server_->InProcessChannel(grpc::ChannelArguments());
    client_ = std::make_unique<SampleServiceClient>(
        fmgr::v1::SampleService::NewStub(channel));
    controller_ = std::make_unique<BarcodeScanController>(client_.get());
    controller_->setToken(QStringLiteral("tok"));
    controller_->setScope(QStringLiteral("lab-1"));
  }

  void TearDown() override {
    if (server_) {
      server_->Shutdown();
      server_->Wait();
    }
  }

  FakeSampleService service_;
  std::unique_ptr<grpc::Server> server_;
  std::unique_ptr<SampleServiceClient> client_;
  std::unique_ptr<BarcodeScanController> controller_;
};

TEST_F(BarcodeScanControllerTest, KnownBarcodeChecksOut) {
  controller_->setAction(fmgr::v1::CHECKOUT_ACTION_CHECKOUT);
  const auto result = controller_->processScan(QStringLiteral("BC-OK"));
  EXPECT_TRUE(result.ok);
  EXPECT_EQ(result.sample_id, QStringLiteral("s-1"));
  EXPECT_TRUE(service_.checkout_called);
  EXPECT_EQ(service_.seen_checkout_id, "s-1");
  EXPECT_EQ(service_.seen_action, fmgr::v1::CHECKOUT_ACTION_CHECKOUT);
}

TEST_F(BarcodeScanControllerTest, UnknownBarcodeDoesNotCheckout) {
  const auto result = controller_->processScan(QStringLiteral("BC-MISSING"));
  EXPECT_FALSE(result.ok);
  EXPECT_FALSE(service_.checkout_called);
  EXPECT_TRUE(result.message.contains(QStringLiteral("BC-MISSING")));
}

TEST_F(BarcodeScanControllerTest, RejectedCheckoutSurfacesError) {
  service_.fail_checkout = true;
  const auto result = controller_->processScan(QStringLiteral("BC-OK"));
  EXPECT_FALSE(result.ok);
  EXPECT_TRUE(service_.checkout_called);
  EXPECT_TRUE(result.message.contains(QStringLiteral("already checked out")));
}

TEST_F(BarcodeScanControllerTest, ActionForwardedToServer) {
  controller_->setAction(fmgr::v1::CHECKOUT_ACTION_CHECKIN);
  controller_->processScan(QStringLiteral("BC-OK"));
  EXPECT_EQ(service_.seen_action, fmgr::v1::CHECKOUT_ACTION_CHECKIN);
}

}  // namespace
