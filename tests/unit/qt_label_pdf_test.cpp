// SPDX-License-Identifier: AGPL-3.0-or-later

#include "qt/LabelPdf.h"

#include <memory>
#include <string>
#include <vector>

#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <gtest/gtest.h>

#include <QByteArray>
#include <QString>

#include "fmgr/v1/sample.grpc.pb.h"
#include "qt/SampleServiceClient.h"

namespace {

using fmgr::qt::LabelPdf;
using fmgr::qt::SampleServiceClient;

// Returns a distinct sample per requested id so multi-label layout is testable.
class FakeSampleService final : public fmgr::v1::SampleService::Service {
 public:
  grpc::Status GetSample(grpc::ServerContext* /*ctx*/,
                         const fmgr::v1::GetSampleRequest* req,
                         fmgr::v1::GetSampleResponse* resp) override {
    fmgr::v1::Sample* s = resp->mutable_sample();
    s->set_id(req->sample_id());
    s->set_name("Sample " + req->sample_id());
    s->set_barcode("BC-" + req->sample_id());
    s->set_box_id("box-9");
    s->set_position_label("C4");
    return grpc::Status::OK;
  }
};

class LabelPdfTest : public ::testing::Test {
 protected:
  void SetUp() override {
    grpc::ServerBuilder builder;
    builder.RegisterService(&sample_service_);
    server_ = builder.BuildAndStart();
    ASSERT_NE(server_, nullptr);
    auto channel = server_->InProcessChannel(grpc::ChannelArguments());
    samples_ = std::make_unique<SampleServiceClient>(
        fmgr::v1::SampleService::NewStub(channel));
    pdf_ = std::make_unique<LabelPdf>(samples_.get());
  }

  void TearDown() override {
    if (server_) {
      server_->Shutdown();
      server_->Wait();
    }
  }

  FakeSampleService sample_service_;
  std::unique_ptr<grpc::Server> server_;
  std::unique_ptr<SampleServiceClient> samples_;
  std::unique_ptr<LabelPdf> pdf_;
};

TEST_F(LabelPdfTest, RendersSingleLabel) {
  const auto m = pdf_->buildModel({QStringLiteral("s-1")},
                                  QStringLiteral("tok"));
  ASSERT_EQ(m.labels.size(), 1u);
  EXPECT_EQ(m.labels[0].name, QStringLiteral("Sample s-1"));
  EXPECT_TRUE(m.labels[0].barcode.contains(QStringLiteral("BC-s-1")));
  EXPECT_TRUE(m.labels[0].location.contains(QStringLiteral("box-9")));
  EXPECT_TRUE(m.labels[0].location.contains(QStringLiteral("C4")));

  const QByteArray bytes =
      pdf_->generate({QStringLiteral("s-1")}, QStringLiteral("tok"));
  EXPECT_TRUE(bytes.startsWith("%PDF"));
}

TEST_F(LabelPdfTest, RendersMultipleLabels) {
  const std::vector<QString> ids = {
      QStringLiteral("s-1"), QStringLiteral("s-2"), QStringLiteral("s-3")};
  const auto m = pdf_->buildModel(ids, QStringLiteral("tok"));
  EXPECT_EQ(m.labels.size(), 3u);

  const QByteArray bytes = pdf_->generate(ids, QStringLiteral("tok"));
  EXPECT_TRUE(bytes.startsWith("%PDF"));
  EXPECT_FALSE(bytes.isEmpty());
}

TEST_F(LabelPdfTest, EmptySampleListReturnsEmptyPdf) {
  const auto m = pdf_->buildModel({}, QStringLiteral("tok"));
  EXPECT_TRUE(m.labels.empty());

  const QByteArray bytes = pdf_->generate({}, QStringLiteral("tok"));
  EXPECT_TRUE(bytes.startsWith("%PDF"));
}

}  // namespace
