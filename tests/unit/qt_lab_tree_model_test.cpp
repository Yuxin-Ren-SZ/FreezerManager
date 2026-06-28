// SPDX-License-Identifier: AGPL-3.0-or-later

#include "qt/LabTreeModel.h"

#include <memory>
#include <string>

#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <gtest/gtest.h>

#include "fmgr/v1/box.grpc.pb.h"
#include "fmgr/v1/lab.grpc.pb.h"
#include "qt/BoxServiceClient.h"
#include "qt/LabServiceClient.h"

namespace {

using fmgr::qt::BoxServiceClient;
using fmgr::qt::LabServiceClient;
using fmgr::qt::TreeNode;

// Lab + Box fakes wired together so buildLabTree can be exercised headlessly
// (no QApplication / QWidget). Topology produced:
//   lab-1 "Alpha" ─ frz-1 ─ ctr-root child: shelf-1 ─ drawer-1
//                 └ frz-2 (no containers)
//   lab-2 "Beta"  (no freezers)
class FakeLabService final : public fmgr::v1::LabService::Service {
 public:
  bool fail_list = false;

  grpc::Status ListLabs(grpc::ServerContext* /*ctx*/,
                        const fmgr::v1::ListLabsRequest* /*req*/,
                        fmgr::v1::ListLabsResponse* resp) override {
    if (fail_list) {
      return {grpc::StatusCode::PERMISSION_DENIED, "nope"};
    }
    fmgr::v1::Lab* a = resp->add_labs();
    a->set_id("lab-1");
    a->set_name("Alpha");
    fmgr::v1::Lab* b = resp->add_labs();
    b->set_id("lab-2");
    b->set_name("Beta");
    return grpc::Status::OK;
  }
};

class FakeBoxService final : public fmgr::v1::BoxService::Service {
 public:
  grpc::Status ListFreezers(grpc::ServerContext* /*ctx*/,
                            const fmgr::v1::ListFreezersRequest* req,
                            fmgr::v1::ListFreezersResponse* resp) override {
    if (req->lab_id() == "lab-1") {
      fmgr::v1::Freezer* f1 = resp->add_freezers();
      f1->set_id("frz-1");
      f1->set_lab_id("lab-1");
      f1->set_name("Minus80 A");
      f1->set_layout_root_id("ctr-root");
      fmgr::v1::Freezer* f2 = resp->add_freezers();
      f2->set_id("frz-2");
      f2->set_lab_id("lab-1");
      f2->set_name("Minus80 B");
      f2->set_layout_root_id("ctr-empty");
    }
    return grpc::Status::OK;
  }

  grpc::Status ListStorageContainers(
      grpc::ServerContext* /*ctx*/,
      const fmgr::v1::ListStorageContainersRequest* req,
      fmgr::v1::ListStorageContainersResponse* resp) override {
    const std::string parent = req->has_parent_id() ? req->parent_id() : "";
    if (parent == "ctr-root") {
      fmgr::v1::StorageContainer* c = resp->add_containers();
      c->set_id("ctr-shelf-1");
      c->set_lab_id("lab-1");
      c->set_parent_id("ctr-root");
      c->set_kind(fmgr::v1::CONTAINER_KIND_SHELF);
      c->set_name("Shelf 1");
      c->set_label("S1");
    } else if (parent == "ctr-shelf-1") {
      fmgr::v1::StorageContainer* c = resp->add_containers();
      c->set_id("ctr-drawer-1");
      c->set_lab_id("lab-1");
      c->set_parent_id("ctr-shelf-1");
      c->set_kind(fmgr::v1::CONTAINER_KIND_DRAWER);
      c->set_name("Drawer 1");  // no label → falls back to name
    }
    return grpc::Status::OK;
  }
};

class LabTreeModelTest : public ::testing::Test {
 protected:
  void SetUp() override {
    grpc::ServerBuilder builder;
    builder.RegisterService(&lab_service_);
    builder.RegisterService(&box_service_);
    server_ = builder.BuildAndStart();
    ASSERT_NE(server_, nullptr);
    auto channel = server_->InProcessChannel(grpc::ChannelArguments());
    labs_ = std::make_unique<LabServiceClient>(
        fmgr::v1::LabService::NewStub(channel));
    boxes_ = std::make_unique<BoxServiceClient>(
        fmgr::v1::BoxService::NewStub(channel));
  }

  void TearDown() override {
    if (server_) {
      server_->Shutdown();
      server_->Wait();
    }
  }

  FakeLabService lab_service_;
  FakeBoxService box_service_;
  std::unique_ptr<grpc::Server> server_;
  std::unique_ptr<LabServiceClient> labs_;
  std::unique_ptr<BoxServiceClient> boxes_;
};

TEST_F(LabTreeModelTest, BuildsLabFreezerContainerHierarchy) {
  auto tree = fmgr::qt::buildLabTree(*labs_, *boxes_, QStringLiteral("tok"));
  ASSERT_TRUE(tree.has_value());
  ASSERT_EQ(tree->size(), 2u);

  // lab-1 Alpha: two freezers.
  const TreeNode& alpha = (*tree)[0];
  EXPECT_EQ(alpha.kind, QStringLiteral("lab"));
  EXPECT_EQ(alpha.id, QStringLiteral("lab-1"));
  EXPECT_EQ(alpha.label, QStringLiteral("Alpha"));
  ASSERT_EQ(alpha.children.size(), 2u);

  // frz-1 → shelf-1 → drawer-1 (recursion).
  const TreeNode& frz1 = alpha.children[0];
  EXPECT_EQ(frz1.kind, QStringLiteral("freezer"));
  EXPECT_EQ(frz1.id, QStringLiteral("frz-1"));
  ASSERT_EQ(frz1.children.size(), 1u);
  const TreeNode& shelf = frz1.children[0];
  EXPECT_EQ(shelf.kind, QStringLiteral("container"));
  EXPECT_EQ(shelf.id, QStringLiteral("ctr-shelf-1"));
  EXPECT_EQ(shelf.label, QStringLiteral("S1"));  // label preferred
  ASSERT_EQ(shelf.children.size(), 1u);
  const TreeNode& drawer = shelf.children[0];
  EXPECT_EQ(drawer.id, QStringLiteral("ctr-drawer-1"));
  EXPECT_EQ(drawer.label, QStringLiteral("Drawer 1"));  // name fallback
  EXPECT_TRUE(drawer.children.empty());

  // frz-2 has an empty root container.
  EXPECT_TRUE(alpha.children[1].children.empty());

  // lab-2 Beta: no freezers.
  EXPECT_TRUE((*tree)[1].children.empty());
}

TEST_F(LabTreeModelTest, ReturnsNulloptWhenLabsListingFails) {
  lab_service_.fail_list = true;
  auto tree = fmgr::qt::buildLabTree(*labs_, *boxes_, QStringLiteral("tok"));
  EXPECT_FALSE(tree.has_value());
}

}  // namespace
