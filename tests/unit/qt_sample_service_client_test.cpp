// SPDX-License-Identifier: AGPL-3.0-or-later

#include "qt/SampleServiceClient.h"

#include <memory>
#include <string>

#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <gtest/gtest.h>

#include "fmgr/v1/sample.grpc.pb.h"

namespace {

using fmgr::qt::SampleFilter;
using fmgr::qt::SampleServiceClient;

// In-process fake SampleService: records the filter/cursor/bearer it received
// and replies with scripted samples + a next-page cursor.
class FakeSampleService final : public fmgr::v1::SampleService::Service {
 public:
  bool fail_list = false;

  // Captured inputs.
  std::string seen_lab_id;
  std::string seen_box_id;
  bool seen_has_box = false;
  std::string seen_barcode;
  bool seen_has_status = false;
  fmgr::v1::SampleStatus seen_status = fmgr::v1::SAMPLE_STATUS_UNSPECIFIED;
  std::string seen_page_token;
  std::string seen_authorization;
  std::string seen_get_id;

  grpc::Status ListSamples(grpc::ServerContext* ctx,
                           const fmgr::v1::ListSamplesRequest* req,
                           fmgr::v1::ListSamplesResponse* resp) override {
    seen_lab_id = req->lab_id();
    seen_has_box = req->has_box_id();
    seen_box_id = req->box_id();
    seen_barcode = req->barcode();
    seen_has_status = req->has_status();
    seen_status = req->status();
    seen_page_token = req->has_page() ? req->page().page_token() : "";
    seen_authorization = metadata(ctx, "authorization");
    if (fail_list) {
      return {grpc::StatusCode::PERMISSION_DENIED, "denied"};
    }

    // Two pages: empty cursor → page 1 + token "p2"; "p2" → page 2, no token.
    if (seen_page_token.empty()) {
      fmgr::v1::Sample* s = resp->add_samples();
      s->set_id("s-1");
      s->set_name("Sample 1");
      s->set_barcode("BC1");
      s->set_status(fmgr::v1::SAMPLE_STATUS_ACTIVE);
      s->set_box_id("box-1");
      s->set_position_label("A1");
      s->set_volume_value(1.5);
      s->set_volume_unit("mL");
      s->set_item_type_id("it-1");
      resp->mutable_page()->set_next_page_token("p2");
    } else if (seen_page_token == "p2") {
      fmgr::v1::Sample* s = resp->add_samples();
      s->set_id("s-2");
      s->set_name("Sample 2");
      s->set_status(fmgr::v1::SAMPLE_STATUS_CHECKED_OUT);
      // no next token → end of stream
    }
    return grpc::Status::OK;
  }

  grpc::Status GetSample(grpc::ServerContext* /*ctx*/,
                         const fmgr::v1::GetSampleRequest* req,
                         fmgr::v1::GetSampleResponse* resp) override {
    seen_get_id = req->sample_id();
    fmgr::v1::Sample* s = resp->mutable_sample();
    s->set_id(req->sample_id());
    s->set_name("Resolved");
    return grpc::Status::OK;
  }

  bool fail_move = false;
  std::string seen_move_id;
  std::string seen_move_box;
  std::string seen_move_position;

  grpc::Status MoveSample(grpc::ServerContext* /*ctx*/,
                          const fmgr::v1::MoveSampleRequest* req,
                          fmgr::v1::MoveSampleResponse* resp) override {
    seen_move_id = req->sample_id();
    seen_move_box = req->has_dest_box_id() ? req->dest_box_id() : "";
    seen_move_position = req->has_dest_position() ? req->dest_position() : "";
    if (fail_move) {
      return {grpc::StatusCode::FAILED_PRECONDITION, "size mismatch"};
    }
    fmgr::v1::Sample* s = resp->mutable_sample();
    s->set_id(req->sample_id());
    s->set_box_id(req->dest_box_id());
    s->set_position_label(req->dest_position());
    return grpc::Status::OK;
  }

  bool fail_checkout = false;
  std::string seen_checkout_id;
  fmgr::v1::CheckoutAction seen_checkout_action = fmgr::v1::CHECKOUT_ACTION_UNSPECIFIED;

  grpc::Status CheckoutSample(grpc::ServerContext* /*ctx*/,
                              const fmgr::v1::CheckoutSampleRequest* req,
                              fmgr::v1::CheckoutSampleResponse* resp) override {
    seen_checkout_id = req->sample_id();
    seen_checkout_action = req->action();
    if (fail_checkout) {
      return {grpc::StatusCode::FAILED_PRECONDITION, "already checked out"};
    }
    fmgr::v1::Sample* s = resp->mutable_sample();
    s->set_id(req->sample_id());
    s->set_status(fmgr::v1::SAMPLE_STATUS_CHECKED_OUT);
    return grpc::Status::OK;
  }

  std::string seen_export_lab;
  bool seen_export_archived = false;

  grpc::Status ExportSamplesCsv(grpc::ServerContext* /*ctx*/,
                                const fmgr::v1::ExportSamplesCsvRequest* req,
                                fmgr::v1::ExportSamplesCsvResponse* resp) override {
    seen_export_lab = req->lab_id();
    seen_export_archived = req->include_archived();
    resp->set_csv_content("id,name\ns-1,Sample 1\n");
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

class SampleServiceClientTest : public ::testing::Test {
 protected:
  void SetUp() override {
    grpc::ServerBuilder builder;
    builder.RegisterService(&service_);
    server_ = builder.BuildAndStart();
    ASSERT_NE(server_, nullptr);
    auto channel = server_->InProcessChannel(grpc::ChannelArguments());
    client_ = std::make_unique<SampleServiceClient>(
        fmgr::v1::SampleService::NewStub(channel));
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
};

TEST_F(SampleServiceClientTest, ListSamplesPassesFiltersAndMapsRows) {
  SampleFilter filter;
  filter.lab_id = QStringLiteral("lab-1");
  filter.box_id = QStringLiteral("box-1");
  filter.barcode = QStringLiteral("BC1");
  filter.status = fmgr::v1::SAMPLE_STATUS_ACTIVE;

  auto result = client_->listSamples(QStringLiteral("tok-1"), filter);

  ASSERT_TRUE(result.ok);
  EXPECT_EQ(service_.seen_lab_id, "lab-1");
  EXPECT_TRUE(service_.seen_has_box);
  EXPECT_EQ(service_.seen_box_id, "box-1");
  EXPECT_EQ(service_.seen_barcode, "BC1");
  EXPECT_TRUE(service_.seen_has_status);
  EXPECT_EQ(service_.seen_status, fmgr::v1::SAMPLE_STATUS_ACTIVE);
  EXPECT_EQ(service_.seen_authorization, "Bearer tok-1");

  ASSERT_EQ(result.samples.size(), 1u);
  const auto& row = result.samples[0];
  EXPECT_EQ(row.id, QStringLiteral("s-1"));
  EXPECT_EQ(row.name, QStringLiteral("Sample 1"));
  EXPECT_EQ(row.barcode, QStringLiteral("BC1"));
  EXPECT_EQ(row.status, fmgr::v1::SAMPLE_STATUS_ACTIVE);
  EXPECT_EQ(row.box_id, QStringLiteral("box-1"));
  EXPECT_EQ(row.position_label, QStringLiteral("A1"));
  ASSERT_TRUE(row.volume_value.has_value());
  EXPECT_DOUBLE_EQ(*row.volume_value, 1.5);
  EXPECT_EQ(row.volume_unit, QStringLiteral("mL"));
  EXPECT_EQ(result.next_page_token, QStringLiteral("p2"));
}

TEST_F(SampleServiceClientTest, ListSamplesForwardsCursor) {
  SampleFilter filter;
  filter.lab_id = QStringLiteral("lab-1");
  auto result =
      client_->listSamples(QStringLiteral("tok-1"), filter, QStringLiteral("p2"));
  ASSERT_TRUE(result.ok);
  EXPECT_EQ(service_.seen_page_token, "p2");
  ASSERT_EQ(result.samples.size(), 1u);
  EXPECT_EQ(result.samples[0].id, QStringLiteral("s-2"));
  EXPECT_TRUE(result.next_page_token.isEmpty());
}

TEST_F(SampleServiceClientTest, ListSamplesOmitsUnsetFilters) {
  SampleFilter filter;
  filter.lab_id = QStringLiteral("lab-1");
  client_->listSamples(QStringLiteral("tok-1"), filter);
  EXPECT_FALSE(service_.seen_has_box);
  EXPECT_FALSE(service_.seen_has_status);
  EXPECT_TRUE(service_.seen_barcode.empty());
}

TEST_F(SampleServiceClientTest, ListSamplesFailureCarriesError) {
  service_.fail_list = true;
  SampleFilter filter;
  filter.lab_id = QStringLiteral("lab-1");
  auto result = client_->listSamples(QStringLiteral("tok-1"), filter);
  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.error, "denied");
  EXPECT_TRUE(result.samples.empty());
}

TEST_F(SampleServiceClientTest, CheckoutSamplePassesAction) {
  auto result = client_->checkoutSample(QStringLiteral("tok-1"),
                                        QStringLiteral("s-1"),
                                        fmgr::v1::CHECKOUT_ACTION_CHECKOUT);
  ASSERT_TRUE(result.ok);
  EXPECT_EQ(service_.seen_checkout_id, "s-1");
  EXPECT_EQ(service_.seen_checkout_action, fmgr::v1::CHECKOUT_ACTION_CHECKOUT);
  EXPECT_EQ(result.sample.status, fmgr::v1::SAMPLE_STATUS_CHECKED_OUT);
}

TEST_F(SampleServiceClientTest, CheckoutSampleRejectionCarriesError) {
  service_.fail_checkout = true;
  auto result = client_->checkoutSample(QStringLiteral("tok-1"),
                                        QStringLiteral("s-1"),
                                        fmgr::v1::CHECKOUT_ACTION_CHECKIN);
  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.error, "already checked out");
}

TEST_F(SampleServiceClientTest, ExportSamplesCsvMapsContent) {
  auto result = client_->exportSamplesCsv(QStringLiteral("tok-1"),
                                          QStringLiteral("lab-1"),
                                          /*include_archived=*/true);
  ASSERT_TRUE(result.ok);
  EXPECT_EQ(service_.seen_export_lab, "lab-1");
  EXPECT_TRUE(service_.seen_export_archived);
  EXPECT_EQ(result.csv, QStringLiteral("id,name\ns-1,Sample 1\n"));
}

TEST_F(SampleServiceClientTest, GetSamplePassesId) {
  auto result =
      client_->getSample(QStringLiteral("tok-1"), QStringLiteral("s-9"));
  ASSERT_TRUE(result.ok);
  EXPECT_EQ(service_.seen_get_id, "s-9");
  EXPECT_EQ(result.sample.id, QStringLiteral("s-9"));
  EXPECT_EQ(result.sample.name, QStringLiteral("Resolved"));
}

TEST_F(SampleServiceClientTest, MoveSamplePassesDestination) {
  auto result = client_->moveSample(QStringLiteral("tok-1"),
                                    QStringLiteral("s-1"),
                                    QStringLiteral("box-2"),
                                    QStringLiteral("B3"));
  ASSERT_TRUE(result.ok);
  EXPECT_EQ(service_.seen_move_id, "s-1");
  EXPECT_EQ(service_.seen_move_box, "box-2");
  EXPECT_EQ(service_.seen_move_position, "B3");
  EXPECT_EQ(result.sample.box_id, QStringLiteral("box-2"));
  EXPECT_EQ(result.sample.position_label, QStringLiteral("B3"));
}

TEST_F(SampleServiceClientTest, MoveSampleRejectionCarriesError) {
  service_.fail_move = true;
  auto result = client_->moveSample(QStringLiteral("tok-1"),
                                    QStringLiteral("s-1"),
                                    QStringLiteral("box-2"),
                                    QStringLiteral("B3"));
  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.error, "size mismatch");
}

}  // namespace
