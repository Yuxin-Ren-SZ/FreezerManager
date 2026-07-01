// SPDX-License-Identifier: AGPL-3.0-or-later

#include "qt/BoxServiceClient.h"

#include <memory>
#include <string>

#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <gtest/gtest.h>

#include "fmgr/v1/box.grpc.pb.h"

namespace {

  using fmgr::qt::BoxServiceClient;

  // In-process fake BoxService covering the two layout read RPCs the Qt tree
  // browser consumes. Records request scoping (lab_id, parent_id) and the bearer.
  // Error-injection: set any `fail_*` field to a non-OK gRPC status to exercise
  // the client's error-handling branches.
  class FakeBoxService final : public fmgr::v1::BoxService::Service {
  public:
    std::string seen_freezers_lab_id;
    std::string seen_freezers_authorization;
    std::string seen_containers_lab_id;
    bool seen_containers_has_parent = false;
    std::string seen_containers_parent_id;

    // Per-RPC error injection. When a fail_* status is set (non-zero error code),
    // the corresponding handler returns that status instead of OK.
    grpc::StatusCode fail_list_freezers = grpc::StatusCode::OK;
    grpc::StatusCode fail_list_containers = grpc::StatusCode::OK;
    grpc::StatusCode fail_list_boxes = grpc::StatusCode::OK;
    grpc::StatusCode fail_get_box = grpc::StatusCode::OK;
    grpc::StatusCode fail_list_box_types = grpc::StatusCode::OK;
    std::string fail_message; // appended to the status message when non-OK

    grpc::Status ListFreezers(grpc::ServerContext* ctx, const fmgr::v1::ListFreezersRequest* req,
                              fmgr::v1::ListFreezersResponse* resp) override {
      if (fail_list_freezers != grpc::StatusCode::OK) {
        return {fail_list_freezers, "injected ListFreezers failure: " + fail_message};
      }
      seen_freezers_lab_id = req->lab_id();
      seen_freezers_authorization = metadata(ctx, "authorization");
      fmgr::v1::Freezer* f = resp->add_freezers();
      f->set_id("frz-1");
      f->set_lab_id(req->lab_id());
      f->set_name("Minus80 A");
      f->set_location("Room 214");
      f->set_model("Thermo TSX");
      f->set_layout_root_id("ctr-root");
      return grpc::Status::OK;
    }

    std::string seen_boxes_lab_id;
    std::string seen_boxes_container_id;

    grpc::Status ListBoxes(grpc::ServerContext* /*ctx*/, const fmgr::v1::ListBoxesRequest* req,
                           fmgr::v1::ListBoxesResponse* resp) override {
      if (fail_list_boxes != grpc::StatusCode::OK) {
        return {fail_list_boxes, "injected ListBoxes failure: " + fail_message};
      }
      seen_boxes_lab_id = req->lab_id();
      seen_boxes_container_id = req->has_storage_container_id() ? req->storage_container_id() : "";
      fmgr::v1::Box* b = resp->add_boxes();
      b->set_id("box-1");
      b->set_lab_id(req->lab_id());
      b->set_storage_container_id(seen_boxes_container_id);
      b->set_label("Cryobox 1");
      return grpc::Status::OK;
    }

    std::string seen_get_box_id;

    grpc::Status GetBox(grpc::ServerContext* /*ctx*/, const fmgr::v1::GetBoxRequest* req,
                        fmgr::v1::GetBoxResponse* resp) override {
      if (fail_get_box != grpc::StatusCode::OK) {
        return {fail_get_box, "injected GetBox failure: " + fail_message};
      }
      seen_get_box_id = req->box_id();
      fmgr::v1::Box* b = resp->mutable_box();
      b->set_id(req->box_id());
      b->set_lab_id("lab-1");
      b->set_box_type_id("bt-1");
      b->set_label("Cryobox 1");
      return grpc::Status::OK;
    }

    grpc::Status ListBoxTypes(grpc::ServerContext* /*ctx*/,
                              const fmgr::v1::ListBoxTypesRequest* /*req*/,
                              fmgr::v1::ListBoxTypesResponse* resp) override {
      if (fail_list_box_types != grpc::StatusCode::OK) {
        return {fail_list_box_types, "injected ListBoxTypes failure: " + fail_message};
      }
      fmgr::v1::BoxType* bt = resp->add_box_types();
      bt->set_id("bt-1");
      bt->set_name("9x9 Cryobox");
      fmgr::v1::BoxPosition* p = bt->add_positions();
      p->set_label("A1");
      p->set_row(0);
      p->set_col(0);
      p->add_accepts("cryovial_2mL");
      return grpc::Status::OK;
    }

    grpc::Status ListStorageContainers(grpc::ServerContext* /*ctx*/,
                                       const fmgr::v1::ListStorageContainersRequest* req,
                                       fmgr::v1::ListStorageContainersResponse* resp) override {
      if (fail_list_containers != grpc::StatusCode::OK) {
        return {fail_list_containers, "injected ListContainers failure: " + fail_message};
      }
      seen_containers_lab_id = req->lab_id();
      seen_containers_has_parent = req->has_parent_id();
      seen_containers_parent_id = req->parent_id();
      // Only return children for the requested root; deeper levels are empty so
      // the recursion in the tree terminates.
      if (req->has_parent_id() && req->parent_id() == "ctr-root") {
        fmgr::v1::StorageContainer* c = resp->add_containers();
        c->set_id("ctr-shelf-1");
        c->set_lab_id(req->lab_id());
        c->set_parent_id("ctr-root");
        c->set_kind(fmgr::v1::CONTAINER_KIND_SHELF);
        c->set_name("Shelf 1");
        c->set_label("S1");
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

  class BoxServiceClientTest : public ::testing::Test {
  protected:
    void SetUp() override {
      grpc::ServerBuilder builder;
      builder.RegisterService(&service_);
      server_ = builder.BuildAndStart();
      ASSERT_NE(server_, nullptr);
      auto channel = server_->InProcessChannel(grpc::ChannelArguments());
      client_ = std::make_unique<BoxServiceClient>(fmgr::v1::BoxService::NewStub(channel));
    }

    void TearDown() override {
      if (server_) {
        server_->Shutdown();
        server_->Wait();
      }
    }

    FakeBoxService service_;
    std::unique_ptr<grpc::Server> server_;
    std::unique_ptr<BoxServiceClient> client_;
  };

  TEST_F(BoxServiceClientTest, ListFreezersMapsRowsAndScopesToLab) {
    auto result = client_->listFreezers(QStringLiteral("tok-1"), QStringLiteral("lab-1"));

    ASSERT_TRUE(result.ok);
    EXPECT_EQ(service_.seen_freezers_lab_id, "lab-1");
    EXPECT_EQ(service_.seen_freezers_authorization, "Bearer tok-1");
    ASSERT_EQ(result.freezers.size(), 1u);
    EXPECT_EQ(result.freezers[0].id, QStringLiteral("frz-1"));
    EXPECT_EQ(result.freezers[0].name, QStringLiteral("Minus80 A"));
    EXPECT_EQ(result.freezers[0].layout_root_id, QStringLiteral("ctr-root"));
  }

  TEST_F(BoxServiceClientTest, ListContainersPassesParentFilter) {
    auto result = client_->listStorageContainers(QStringLiteral("tok-1"), QStringLiteral("lab-1"),
                                                 QStringLiteral("ctr-root"));

    ASSERT_TRUE(result.ok);
    EXPECT_EQ(service_.seen_containers_lab_id, "lab-1");
    EXPECT_TRUE(service_.seen_containers_has_parent);
    EXPECT_EQ(service_.seen_containers_parent_id, "ctr-root");
    ASSERT_EQ(result.containers.size(), 1u);
    EXPECT_EQ(result.containers[0].id, QStringLiteral("ctr-shelf-1"));
    ASSERT_TRUE(result.containers[0].parent_id.has_value());
    EXPECT_EQ(*result.containers[0].parent_id, QStringLiteral("ctr-root"));
    EXPECT_EQ(result.containers[0].kind, fmgr::v1::CONTAINER_KIND_SHELF);
  }

  TEST_F(BoxServiceClientTest, ListContainersWithoutParentOmitsFilter) {
    auto result = client_->listStorageContainers(QStringLiteral("tok-1"), QStringLiteral("lab-1"));
    ASSERT_TRUE(result.ok);
    EXPECT_FALSE(service_.seen_containers_has_parent);
    EXPECT_TRUE(result.containers.empty());
  }

  TEST_F(BoxServiceClientTest, ListBoxesScopesToContainer) {
    auto result = client_->listBoxes(QStringLiteral("tok-1"), QStringLiteral("lab-1"),
                                     QStringLiteral("ctr-shelf-1"));
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(service_.seen_boxes_lab_id, "lab-1");
    EXPECT_EQ(service_.seen_boxes_container_id, "ctr-shelf-1");
    ASSERT_EQ(result.boxes.size(), 1u);
    EXPECT_EQ(result.boxes[0].id, QStringLiteral("box-1"));
    EXPECT_EQ(result.boxes[0].label, QStringLiteral("Cryobox 1"));
    EXPECT_EQ(result.boxes[0].storage_container_id, QStringLiteral("ctr-shelf-1"));
  }

  TEST_F(BoxServiceClientTest, GetBoxCarriesBoxTypeId) {
    auto result = client_->getBox(QStringLiteral("tok-1"), QStringLiteral("box-1"));
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(service_.seen_get_box_id, "box-1");
    EXPECT_EQ(result.box.id, QStringLiteral("box-1"));
    EXPECT_EQ(result.box.box_type_id, QStringLiteral("bt-1"));
  }

  TEST_F(BoxServiceClientTest, ListBoxTypesMapsPositions) {
    auto result = client_->listBoxTypes(QStringLiteral("tok-1"), QStringLiteral("lab-1"));
    ASSERT_TRUE(result.ok);
    ASSERT_EQ(result.box_types.size(), 1u);
    const auto& bt = result.box_types[0];
    EXPECT_EQ(bt.id, QStringLiteral("bt-1"));
    ASSERT_EQ(bt.positions.size(), 1u);
    EXPECT_EQ(bt.positions[0].label, QStringLiteral("A1"));
    EXPECT_EQ(bt.positions[0].row, 0);
    EXPECT_EQ(bt.positions[0].col, 0);
    ASSERT_EQ(bt.positions[0].accepts.size(), 1u);
    EXPECT_EQ(bt.positions[0].accepts[0], QStringLiteral("cryovial_2mL"));
  }

  // ── Error-path tests ─────────────────────────────────────────────────

  TEST_F(BoxServiceClientTest, ListFreezersSurfacesGrpcFailure) {
    service_.fail_list_freezers = grpc::StatusCode::UNAVAILABLE;
    service_.fail_message = "server restarting";

    auto result = client_->listFreezers(QStringLiteral("tok-1"), QStringLiteral("lab-1"));
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("injected ListFreezers failure"), std::string::npos);
    EXPECT_TRUE(result.freezers.empty());
  }

  TEST_F(BoxServiceClientTest, ListContainersSurfacesGrpcFailure) {
    service_.fail_list_containers = grpc::StatusCode::PERMISSION_DENIED;
    service_.fail_message = "no lab access";

    auto result = client_->listStorageContainers(QStringLiteral("tok-1"), QStringLiteral("lab-1"));
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("injected ListContainers failure"), std::string::npos);
    EXPECT_TRUE(result.containers.empty());
  }

  TEST_F(BoxServiceClientTest, ListBoxesSurfacesGrpcFailure) {
    service_.fail_list_boxes = grpc::StatusCode::NOT_FOUND;

    auto result = client_->listBoxes(QStringLiteral("tok-1"), QStringLiteral("lab-1"),
                                     QStringLiteral("ghost-container"));
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.boxes.empty());
  }

  TEST_F(BoxServiceClientTest, GetBoxSurfacesGrpcFailure) {
    service_.fail_get_box = grpc::StatusCode::NOT_FOUND;
    service_.fail_message = "box deleted";

    auto result = client_->getBox(QStringLiteral("tok-1"), QStringLiteral("ghost-box"));
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("injected GetBox failure"), std::string::npos);
    EXPECT_TRUE(result.box.id.isEmpty());
  }

  TEST_F(BoxServiceClientTest, ListBoxTypesSurfacesGrpcFailure) {
    service_.fail_list_box_types = grpc::StatusCode::UNAVAILABLE;

    auto result = client_->listBoxTypes(QStringLiteral("tok-1"), QStringLiteral("lab-1"));
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("injected ListBoxTypes failure"), std::string::npos);
    EXPECT_TRUE(result.box_types.empty());
  }

} // namespace
