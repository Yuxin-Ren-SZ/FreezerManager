// SPDX-License-Identifier: AGPL-3.0-or-later

#include "qt/SampleTableModel.h"

#include <memory>
#include <string>

#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <gtest/gtest.h>

#include "fmgr/v1/sample.grpc.pb.h"
#include "qt/SampleServiceClient.h"

namespace {

  using fmgr::qt::SampleFilter;
  using fmgr::qt::SampleServiceClient;
  using fmgr::qt::SampleTableModel;

  // Two-page fake so the model's canFetchMore/fetchMore paging can be exercised.
  class FakeSampleService final : public fmgr::v1::SampleService::Service {
  public:
    grpc::Status ListSamples(grpc::ServerContext* /*ctx*/, const fmgr::v1::ListSamplesRequest* req,
                             fmgr::v1::ListSamplesResponse* resp) override {
      const std::string token = req->has_page() ? req->page().page_token() : "";
      if (token.empty()) {
        fmgr::v1::Sample* s = resp->add_samples();
        s->set_id("s-1");
        s->set_name("Sample 1");
        s->set_status(fmgr::v1::SAMPLE_STATUS_ACTIVE);
        s->set_volume_value(1.5);
        s->set_volume_unit("mL");
        resp->mutable_page()->set_next_page_token("p2");
      } else if (token == "p2") {
        fmgr::v1::Sample* s = resp->add_samples();
        s->set_id("s-2");
        s->set_name("Sample 2");
        s->set_status(fmgr::v1::SAMPLE_STATUS_CHECKED_OUT);
      }
      return grpc::Status::OK;
    }
  };

  class SampleTableModelTest : public ::testing::Test {
  protected:
    void SetUp() override {
      grpc::ServerBuilder builder;
      builder.RegisterService(&service_);
      server_ = builder.BuildAndStart();
      ASSERT_NE(server_, nullptr);
      auto channel = server_->InProcessChannel(grpc::ChannelArguments());
      client_ = std::make_unique<SampleServiceClient>(fmgr::v1::SampleService::NewStub(channel));
      model_ = std::make_unique<SampleTableModel>(client_.get());
      model_->setToken(QStringLiteral("tok"));
    }

    void TearDown() override {
      if (server_) {
        server_->Shutdown();
        server_->Wait();
      }
    }

    SampleFilter labScope() const {
      SampleFilter f;
      f.lab_id = QStringLiteral("lab-1");
      return f;
    }

    FakeSampleService service_;
    std::unique_ptr<grpc::Server> server_;
    std::unique_ptr<SampleServiceClient> client_;
    std::unique_ptr<SampleTableModel> model_;
  };

  TEST_F(SampleTableModelTest, SetScopeLoadsFirstPage) {
    model_->setScope(labScope());
    EXPECT_EQ(model_->rowCount(), 1);
    EXPECT_EQ(model_->columnCount(), SampleTableModel::kColumnCount);
    EXPECT_EQ(model_->data(model_->index(0, SampleTableModel::kName), ::Qt::DisplayRole).toString(),
              QStringLiteral("Sample 1"));
    EXPECT_EQ(
        model_->data(model_->index(0, SampleTableModel::kStatus), ::Qt::DisplayRole).toString(),
        QStringLiteral("Active"));
    EXPECT_EQ(
        model_->data(model_->index(0, SampleTableModel::kVolume), ::Qt::DisplayRole).toString(),
        QStringLiteral("1.5 mL"));
  }

  TEST_F(SampleTableModelTest, PagesInMoreRowsViaFetchMore) {
    model_->setScope(labScope());
    EXPECT_TRUE(model_->canFetchMore(QModelIndex()));

    model_->fetchMore(QModelIndex());
    EXPECT_EQ(model_->rowCount(), 2);
    EXPECT_EQ(model_->data(model_->index(1, SampleTableModel::kName), ::Qt::DisplayRole).toString(),
              QStringLiteral("Sample 2"));

    // Second page carried no cursor → no more pages.
    EXPECT_FALSE(model_->canFetchMore(QModelIndex()));
  }

  TEST_F(SampleTableModelTest, SetScopeResetsRows) {
    model_->setScope(labScope());
    model_->fetchMore(QModelIndex());
    ASSERT_EQ(model_->rowCount(), 2);
    // Re-scoping clears and reloads page 1.
    model_->setScope(labScope());
    EXPECT_EQ(model_->rowCount(), 1);
    EXPECT_TRUE(model_->canFetchMore(QModelIndex()));
  }

  // reload() re-runs the current filter from the first page without changing the
  // scope — the in-place refresh path a live update triggers.
  TEST_F(SampleTableModelTest, ReloadReRunsCurrentFilterFromFirstPage) {
    model_->setScope(labScope());
    model_->fetchMore(QModelIndex());
    ASSERT_EQ(model_->rowCount(), 2);

    model_->reload();
    EXPECT_EQ(model_->rowCount(), 1);
    EXPECT_TRUE(model_->canFetchMore(QModelIndex()));
  }

  TEST_F(SampleTableModelTest, HeaderLabels) {
    EXPECT_EQ(
        model_->headerData(SampleTableModel::kName, ::Qt::Horizontal, ::Qt::DisplayRole).toString(),
        QStringLiteral("Name"));
    EXPECT_EQ(model_->headerData(SampleTableModel::kVolume, ::Qt::Horizontal, ::Qt::DisplayRole)
                  .toString(),
              QStringLiteral("Volume"));
  }

  TEST_F(SampleTableModelTest, StatusToStringMapsKnownStates) {
    EXPECT_EQ(SampleTableModel::statusToString(fmgr::v1::SAMPLE_STATUS_DEPLETED),
              QStringLiteral("Depleted"));
    EXPECT_EQ(SampleTableModel::statusToString(fmgr::v1::SAMPLE_STATUS_TOMBSTONED),
              QStringLiteral("Deleted"));
  }

} // namespace
