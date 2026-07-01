// SPDX-License-Identifier: AGPL-3.0-or-later

#include "qt/BoxGridModel.h"

#include <memory>
#include <string>

#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <gtest/gtest.h>

#include "fmgr/v1/box.grpc.pb.h"
#include "fmgr/v1/sample.grpc.pb.h"
#include "qt/BoxServiceClient.h"
#include "qt/SampleServiceClient.h"

namespace {

using fmgr::qt::BoxGridModel;
using fmgr::qt::BoxServiceClient;
using fmgr::qt::SampleServiceClient;

// Box layout: box-1 is a BoxType bt-1 with two positions A1(0,0), A2(0,1).
class FakeBoxService final : public fmgr::v1::BoxService::Service {
 public:
  grpc::StatusCode fail_get_box = grpc::StatusCode::OK;
  grpc::StatusCode fail_list_box_types = grpc::StatusCode::OK;
  // When set, the returned box type id will differ from the box's box_type_id,
  // testing the "box_type_id not found in list" path.
  bool mismatch_box_type = false;

  grpc::Status GetBox(grpc::ServerContext* /*ctx*/,
                      const fmgr::v1::GetBoxRequest* req,
                      fmgr::v1::GetBoxResponse* resp) override {
    if (fail_get_box != grpc::StatusCode::OK) {
      return {fail_get_box, "injected GetBox failure"};
    }
    fmgr::v1::Box* b = resp->mutable_box();
    b->set_id(req->box_id());
    b->set_lab_id("lab-1");
    b->set_box_type_id("bt-1");
    return grpc::Status::OK;
  }

  grpc::Status ListBoxTypes(grpc::ServerContext* /*ctx*/,
                            const fmgr::v1::ListBoxTypesRequest* /*req*/,
                            fmgr::v1::ListBoxTypesResponse* resp) override {
    if (fail_list_box_types != grpc::StatusCode::OK) {
      return {fail_list_box_types, "injected ListBoxTypes failure"};
    }
    fmgr::v1::BoxType* bt = resp->add_box_types();
    bt->set_id(mismatch_box_type ? "bt-wrong" : "bt-1");
    fmgr::v1::BoxPosition* a1 = bt->add_positions();
    a1->set_label("A1");
    a1->set_row(0);
    a1->set_col(0);
    fmgr::v1::BoxPosition* a2 = bt->add_positions();
    a2->set_label("A2");
    a2->set_row(0);
    a2->set_col(1);
    return grpc::Status::OK;
  }
};

// Sample state: one sample s-1, position mutable by MoveSample.
class FakeSampleService final : public fmgr::v1::SampleService::Service {
 public:
  bool fail_move = false;
  std::string position = "A1";
  grpc::StatusCode fail_list = grpc::StatusCode::OK;

  grpc::Status ListSamples(grpc::ServerContext* /*ctx*/,
                           const fmgr::v1::ListSamplesRequest* /*req*/,
                           fmgr::v1::ListSamplesResponse* resp) override {
    if (fail_list != grpc::StatusCode::OK) {
      return {fail_list, "injected ListSamples failure"};
    }
    fmgr::v1::Sample* s = resp->add_samples();
    s->set_id("s-1");
    s->set_name("Sample 1");
    s->set_box_id("box-1");
    s->set_position_label(position);
    return grpc::Status::OK;
  }

  grpc::Status MoveSample(grpc::ServerContext* /*ctx*/,
                          const fmgr::v1::MoveSampleRequest* req,
                          fmgr::v1::MoveSampleResponse* resp) override {
    if (fail_move) {
      return {grpc::StatusCode::FAILED_PRECONDITION, "size mismatch"};
    }
    position = req->dest_position();
    fmgr::v1::Sample* s = resp->mutable_sample();
    s->set_id(req->sample_id());
    s->set_position_label(position);
    return grpc::Status::OK;
  }
};

class BoxGridModelTest : public ::testing::Test {
 protected:
  void SetUp() override {
    grpc::ServerBuilder builder;
    builder.RegisterService(&box_service_);
    builder.RegisterService(&sample_service_);
    server_ = builder.BuildAndStart();
    ASSERT_NE(server_, nullptr);
    auto channel = server_->InProcessChannel(grpc::ChannelArguments());
    boxes_ = std::make_unique<BoxServiceClient>(
        fmgr::v1::BoxService::NewStub(channel));
    samples_ = std::make_unique<SampleServiceClient>(
        fmgr::v1::SampleService::NewStub(channel));
    model_ = std::make_unique<BoxGridModel>(boxes_.get(), samples_.get());
    model_->setToken(QStringLiteral("tok"));
  }

  void TearDown() override {
    if (server_) {
      server_->Shutdown();
      server_->Wait();
    }
  }

  const BoxGridModel::GridCell* cell(const QString& label) const {
    for (const auto& c : model_->cells()) {
      if (c.position_label == label) {
        return &c;
      }
    }
    return nullptr;
  }

  FakeBoxService box_service_;
  FakeSampleService sample_service_;
  std::unique_ptr<grpc::Server> server_;
  std::unique_ptr<BoxServiceClient> boxes_;
  std::unique_ptr<SampleServiceClient> samples_;
  std::unique_ptr<BoxGridModel> model_;
};

TEST_F(BoxGridModelTest, SetBoxBuildsCellsWithOccupants) {
  ASSERT_TRUE(model_->setBox(QStringLiteral("lab-1"), QStringLiteral("box-1")));
  EXPECT_EQ(model_->cells().size(), 2u);
  EXPECT_EQ(model_->rows(), 1);
  EXPECT_EQ(model_->cols(), 2);

  const auto* a1 = cell(QStringLiteral("A1"));
  ASSERT_NE(a1, nullptr);
  ASSERT_TRUE(a1->occupant.has_value());
  EXPECT_EQ(a1->occupant->sample_id, QStringLiteral("s-1"));
  EXPECT_EQ(a1->occupant->name, QStringLiteral("Sample 1"));

  const auto* a2 = cell(QStringLiteral("A2"));
  ASSERT_NE(a2, nullptr);
  EXPECT_FALSE(a2->occupant.has_value());
}

TEST_F(BoxGridModelTest, MoveSampleRelocatesOccupantOnReload) {
  ASSERT_TRUE(model_->setBox(QStringLiteral("lab-1"), QStringLiteral("box-1")));

  const auto result = model_->moveSample(QStringLiteral("s-1"),
                                         QStringLiteral("A2"));
  ASSERT_TRUE(result.ok);

  EXPECT_FALSE(cell(QStringLiteral("A1"))->occupant.has_value());
  ASSERT_TRUE(cell(QStringLiteral("A2"))->occupant.has_value());
  EXPECT_EQ(cell(QStringLiteral("A2"))->occupant->sample_id,
            QStringLiteral("s-1"));
}

TEST_F(BoxGridModelTest, RejectedMoveKeepsGridUnchanged) {
  ASSERT_TRUE(model_->setBox(QStringLiteral("lab-1"), QStringLiteral("box-1")));
  sample_service_.fail_move = true;

  const auto result = model_->moveSample(QStringLiteral("s-1"),
                                         QStringLiteral("A2"));
  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.error, "size mismatch");

  // Occupant stays at A1.
  ASSERT_TRUE(cell(QStringLiteral("A1"))->occupant.has_value());
  EXPECT_FALSE(cell(QStringLiteral("A2"))->occupant.has_value());
}

// ── Error-path and edge-case tests ──────────────────────────────────

TEST_F(BoxGridModelTest, SetBoxFailsWhenGetBoxReturnsError) {
  box_service_.fail_get_box = grpc::StatusCode::NOT_FOUND;
  EXPECT_FALSE(model_->setBox(QStringLiteral("lab-1"), QStringLiteral("box-1")));
  EXPECT_TRUE(model_->cells().empty());
}

TEST_F(BoxGridModelTest, SetBoxFailsWhenListBoxTypesReturnsError) {
  box_service_.fail_list_box_types = grpc::StatusCode::UNAVAILABLE;
  EXPECT_FALSE(model_->setBox(QStringLiteral("lab-1"), QStringLiteral("box-1")));
  EXPECT_TRUE(model_->cells().empty());
}

TEST_F(BoxGridModelTest, SetBoxFailsWhenBoxTypeIdNotFound) {
  box_service_.mismatch_box_type = true;
  EXPECT_FALSE(model_->setBox(QStringLiteral("lab-1"), QStringLiteral("box-1")));
  EXPECT_TRUE(model_->cells().empty());
}

TEST_F(BoxGridModelTest, AccessorsReturnCorrectIds) {
  ASSERT_TRUE(model_->setBox(QStringLiteral("lab-1"), QStringLiteral("box-1")));
  EXPECT_EQ(model_->labId(), QStringLiteral("lab-1"));
  EXPECT_EQ(model_->boxId(), QStringLiteral("box-1"));
  EXPECT_EQ(model_->token(), QStringLiteral("tok"));
}

TEST_F(BoxGridModelTest, EmptyBoxHasCellsButNoOccupants) {
  // No samples seeded → every position should be free.
  sample_service_.position = "NONE";  // no sample will match this position
  ASSERT_TRUE(model_->setBox(QStringLiteral("lab-1"), QStringLiteral("box-1")));
  EXPECT_EQ(model_->cells().size(), 2u);
  for (const auto& c : model_->cells()) {
    EXPECT_FALSE(c.occupant.has_value());
  }
}

}  // namespace
