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

    grpc::Status ListLabs(grpc::ServerContext* /*ctx*/, const fmgr::v1::ListLabsRequest* /*req*/,
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
    // Per-key error injection so mid-tree RPC failures are exercised: a matching
    // key makes exactly that RPC fail while sibling calls still succeed.
    std::string fail_freezers_for_lab;
    std::string fail_containers_for_parent;
    std::string fail_boxes_for_container;
    // When set, ListStorageContainers returns a cyclic adjacency list
    // (ctr-root → ctr-a → ctr-b → ctr-a …) so the cycle guard can be exercised.
    bool cyclic_containers = false;

    grpc::Status ListFreezers(grpc::ServerContext* /*ctx*/,
                              const fmgr::v1::ListFreezersRequest* req,
                              fmgr::v1::ListFreezersResponse* resp) override {
      if (!fail_freezers_for_lab.empty() && req->lab_id() == fail_freezers_for_lab) {
        return {grpc::StatusCode::UNAVAILABLE, "freezers down"};
      }
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

    grpc::Status ListStorageContainers(grpc::ServerContext* /*ctx*/,
                                       const fmgr::v1::ListStorageContainersRequest* req,
                                       fmgr::v1::ListStorageContainersResponse* resp) override {
      const std::string parent = req->has_parent_id() ? req->parent_id() : "";
      if (!fail_containers_for_parent.empty() && parent == fail_containers_for_parent) {
        return {grpc::StatusCode::UNAVAILABLE, "containers down"};
      }
      if (cyclic_containers) {
        // ctr-root → ctr-a → ctr-b → ctr-a (a back-edge into the ancestry).
        auto add_container = [&](const char* id, const char* par) {
          fmgr::v1::StorageContainer* c = resp->add_containers();
          c->set_id(id);
          c->set_lab_id("lab-1");
          c->set_parent_id(par);
          c->set_kind(fmgr::v1::CONTAINER_KIND_SHELF);
          c->set_name(id);
        };
        if (parent == "ctr-root") {
          add_container("ctr-a", "ctr-root");
        } else if (parent == "ctr-a") {
          add_container("ctr-b", "ctr-a");
        } else if (parent == "ctr-b") {
          add_container("ctr-a", "ctr-b"); // back-edge: ctr-a already on path
        }
        return grpc::Status::OK;
      }
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
        c->set_name("Drawer 1"); // no label → falls back to name
      }
      return grpc::Status::OK;
    }

    grpc::Status ListBoxes(grpc::ServerContext* /*ctx*/, const fmgr::v1::ListBoxesRequest* req,
                           fmgr::v1::ListBoxesResponse* resp) override {
      if (!fail_boxes_for_container.empty() && req->has_storage_container_id() &&
          req->storage_container_id() == fail_boxes_for_container) {
        return {grpc::StatusCode::UNAVAILABLE, "boxes down"};
      }
      // A single box lives in the drawer; other containers have none.
      if (req->has_storage_container_id() && req->storage_container_id() == "ctr-drawer-1") {
        fmgr::v1::Box* b = resp->add_boxes();
        b->set_id("box-1");
        b->set_lab_id("lab-1");
        b->set_storage_container_id("ctr-drawer-1");
        b->set_label("Cryobox 1");
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
      labs_ = std::make_unique<LabServiceClient>(fmgr::v1::LabService::NewStub(channel));
      boxes_ = std::make_unique<BoxServiceClient>(fmgr::v1::BoxService::NewStub(channel));
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
    EXPECT_EQ(alpha.lab_id, QStringLiteral("lab-1"));
    EXPECT_EQ(alpha.label, QStringLiteral("Alpha"));
    ASSERT_EQ(alpha.children.size(), 2u);

    // frz-1 → shelf-1 → drawer-1 → box-1 (recursion + box leaf).
    const TreeNode& frz1 = alpha.children[0];
    EXPECT_EQ(frz1.kind, QStringLiteral("freezer"));
    EXPECT_EQ(frz1.id, QStringLiteral("frz-1"));
    EXPECT_EQ(frz1.lab_id, QStringLiteral("lab-1"));
    ASSERT_EQ(frz1.children.size(), 1u);
    const TreeNode& shelf = frz1.children[0];
    EXPECT_EQ(shelf.kind, QStringLiteral("container"));
    EXPECT_EQ(shelf.id, QStringLiteral("ctr-shelf-1"));
    EXPECT_EQ(shelf.label, QStringLiteral("S1")); // label preferred
    ASSERT_EQ(shelf.children.size(), 1u);
    const TreeNode& drawer = shelf.children[0];
    EXPECT_EQ(drawer.id, QStringLiteral("ctr-drawer-1"));
    EXPECT_EQ(drawer.label, QStringLiteral("Drawer 1")); // name fallback
    // The drawer holds one box leaf.
    ASSERT_EQ(drawer.children.size(), 1u);
    const TreeNode& box = drawer.children[0];
    EXPECT_EQ(box.kind, QStringLiteral("box"));
    EXPECT_EQ(box.id, QStringLiteral("box-1"));
    EXPECT_EQ(box.lab_id, QStringLiteral("lab-1"));
    EXPECT_EQ(box.label, QStringLiteral("Cryobox 1"));
    EXPECT_TRUE(box.children.empty());

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

  // listFreezers failing for one lab must not abort the whole tree: that lab's
  // subtree is skipped (empty children) while sibling labs still appear.
  TEST_F(LabTreeModelTest, FreezerListingFailureForOneLabSkipsItsSubtree) {
    box_service_.fail_freezers_for_lab = "lab-1";
    auto tree = fmgr::qt::buildLabTree(*labs_, *boxes_, QStringLiteral("tok"));
    ASSERT_TRUE(tree.has_value());
    ASSERT_EQ(tree->size(), 2u);

    const TreeNode& alpha = (*tree)[0];
    EXPECT_EQ(alpha.id, QStringLiteral("lab-1"));
    EXPECT_TRUE(alpha.children.empty()); // freezers RPC failed → no subtree

    const TreeNode& beta = (*tree)[1];
    EXPECT_EQ(beta.id, QStringLiteral("lab-2")); // sibling still present
  }

  // A container-listing failure mid-tree skips only that subtree; ancestors and
  // the failing container itself remain, but its descendants are pruned.
  TEST_F(LabTreeModelTest, ContainerListingFailureMidTreeSkipsSubtree) {
    box_service_.fail_containers_for_parent = "ctr-shelf-1";
    auto tree = fmgr::qt::buildLabTree(*labs_, *boxes_, QStringLiteral("tok"));
    ASSERT_TRUE(tree.has_value());

    const TreeNode& frz1 = (*tree)[0].children[0];
    ASSERT_EQ(frz1.children.size(), 1u);
    const TreeNode& shelf = frz1.children[0];
    EXPECT_EQ(shelf.id, QStringLiteral("ctr-shelf-1"));
    // Recursion under the shelf failed → drawer + its box are gone.
    EXPECT_TRUE(shelf.children.empty());
  }

  // A box-listing failure for a container drops its box leaves but keeps the
  // container node and its container children.
  TEST_F(LabTreeModelTest, BoxListingFailureDropsBoxLeavesOnly) {
    box_service_.fail_boxes_for_container = "ctr-drawer-1";
    auto tree = fmgr::qt::buildLabTree(*labs_, *boxes_, QStringLiteral("tok"));
    ASSERT_TRUE(tree.has_value());

    const TreeNode& drawer = (*tree)[0].children[0].children[0].children[0];
    EXPECT_EQ(drawer.id, QStringLiteral("ctr-drawer-1"));
    EXPECT_TRUE(drawer.children.empty()); // box leaf skipped, drawer survives
  }

  // A cyclic container graph (parent points back into its own ancestry) must not
  // recurse forever. The build terminates, keeps the reachable nodes, and stops
  // descending at the back-edge so the repeated container has no further
  // children. frz-1 root "ctr-root" → ctr-a → ctr-b → ctr-a(no descent).
  TEST_F(LabTreeModelTest, CyclicContainerGraphTerminatesAndBreaksAtBackEdge) {
    box_service_.cyclic_containers = true;
    auto tree = fmgr::qt::buildLabTree(*labs_, *boxes_, QStringLiteral("tok"));
    ASSERT_TRUE(tree.has_value());

    const TreeNode& frz1 = (*tree)[0].children[0];
    ASSERT_EQ(frz1.children.size(), 1u);
    const TreeNode& a = frz1.children[0];
    EXPECT_EQ(a.id, QStringLiteral("ctr-a"));
    ASSERT_EQ(a.children.size(), 1u);
    const TreeNode& b = a.children[0];
    EXPECT_EQ(b.id, QStringLiteral("ctr-b"));
    // b's child is ctr-a again, but ctr-a is already on the path → no descent.
    ASSERT_EQ(b.children.size(), 1u);
    const TreeNode& a_again = b.children[0];
    EXPECT_EQ(a_again.id, QStringLiteral("ctr-a"));
    EXPECT_TRUE(a_again.children.empty()); // cycle guard broke the recursion
  }

} // namespace
