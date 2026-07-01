// SPDX-License-Identifier: AGPL-3.0-or-later

#include "qt/LocationPathResolver.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <gtest/gtest.h>

#include "fmgr/v1/box.grpc.pb.h"
#include "qt/BoxServiceClient.h"

namespace {

  using fmgr::qt::BoxServiceClient;
  using fmgr::qt::LocationPathResolver;
  using Kind = fmgr::qt::LocationPathResolver::PathSegment::Kind;

  fmgr::v1::StorageContainer makeContainer(const std::string& id,
                                           std::optional<std::string> parent_id,
                                           fmgr::v1::ContainerKind kind, const std::string& name) {
    fmgr::v1::StorageContainer c;
    c.set_id(id);
    c.set_lab_id("lab-1");
    if (parent_id.has_value()) {
      c.set_parent_id(*parent_id);
    }
    c.set_kind(kind);
    c.set_name(name);
    return c;
  }

  // In-process fake BoxService. Container listing is driven by explicit roots /
  // children maps (not the proto parent_id field) so tests can craft broken chains
  // — an orphaned parent whose container is still reachable from a root.
  class FakeBoxService final : public fmgr::v1::BoxService::Service {
  public:
    std::map<std::string, fmgr::v1::Box> boxes;
    std::vector<fmgr::v1::Freezer> freezers;
    std::vector<fmgr::v1::StorageContainer> roots;
    std::map<std::string, std::vector<fmgr::v1::StorageContainer>> children;

    grpc::Status GetBox(grpc::ServerContext* /*ctx*/, const fmgr::v1::GetBoxRequest* req,
                        fmgr::v1::GetBoxResponse* resp) override {
      const auto it = boxes.find(req->box_id());
      if (it == boxes.end()) {
        return {grpc::StatusCode::NOT_FOUND, "no such box"};
      }
      *resp->mutable_box() = it->second;
      return grpc::Status::OK;
    }

    grpc::Status ListFreezers(grpc::ServerContext* /*ctx*/,
                              const fmgr::v1::ListFreezersRequest* /*req*/,
                              fmgr::v1::ListFreezersResponse* resp) override {
      for (const auto& f : freezers) {
        *resp->add_freezers() = f;
      }
      return grpc::Status::OK;
    }

    grpc::Status ListStorageContainers(grpc::ServerContext* /*ctx*/,
                                       const fmgr::v1::ListStorageContainersRequest* req,
                                       fmgr::v1::ListStorageContainersResponse* resp) override {
      if (req->has_parent_id()) {
        const auto it = children.find(req->parent_id());
        if (it != children.end()) {
          for (const auto& c : it->second) {
            *resp->add_containers() = c;
          }
        }
      } else {
        for (const auto& c : roots) {
          *resp->add_containers() = c;
        }
      }
      return grpc::Status::OK;
    }
  };

  class LocationPathResolverTest : public ::testing::Test {
  protected:
    void SetUp() override {
      grpc::ServerBuilder builder;
      builder.RegisterService(&service_);
      server_ = builder.BuildAndStart();
      ASSERT_NE(server_, nullptr);
      auto channel = server_->InProcessChannel(grpc::ChannelArguments());
      client_ = std::make_unique<BoxServiceClient>(fmgr::v1::BoxService::NewStub(channel));
      resolver_ = std::make_unique<LocationPathResolver>(client_.get());
    }

    void TearDown() override {
      if (server_) {
        server_->Shutdown();
        server_->Wait();
      }
    }

    fmgr::v1::Box makeBox(const std::string& id, const std::string& container,
                          const std::string& label) {
      fmgr::v1::Box b;
      b.set_id(id);
      b.set_lab_id("lab-1");
      b.set_storage_container_id(container);
      b.set_label(label);
      return b;
    }

    LocationPathResolver::Result resolve(const std::string& box_id, const std::string& pos) {
      return resolver_->resolve(QStringLiteral("tok"), QStringLiteral("lab-1"),
                                QString::fromStdString(box_id), QString::fromStdString(pos));
    }

    FakeBoxService service_;
    std::unique_ptr<grpc::Server> server_;
    std::unique_ptr<BoxServiceClient> client_;
    std::unique_ptr<LocationPathResolver> resolver_;
  };

  TEST_F(LocationPathResolverTest, ResolvesFullPath) {
    // Freezer F → Shelf (root) → Rack → Box B [A1].
    fmgr::v1::Freezer f;
    f.set_id("frz-1");
    f.set_lab_id("lab-1");
    f.set_name("Freezer A");
    f.set_layout_root_id("shelf-1");
    service_.freezers.push_back(f);

    service_.roots.push_back(
        makeContainer("shelf-1", std::nullopt, fmgr::v1::CONTAINER_KIND_SHELF, "Shelf"));
    service_.children["shelf-1"].push_back(
        makeContainer("rack-1", "shelf-1", fmgr::v1::CONTAINER_KIND_RACK, "Rack 1"));
    service_.boxes["box-1"] = makeBox("box-1", "rack-1", "Box 7");

    const auto r = resolve("box-1", "A1");
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(r.placed);
    EXPECT_FALSE(r.partial);
    ASSERT_EQ(r.segments.size(), 5);
    EXPECT_EQ(r.segments[0].kind, Kind::Freezer);
    EXPECT_EQ(r.segments[0].label, QStringLiteral("Freezer A"));
    EXPECT_EQ(r.segments[1].kind, Kind::Container);
    EXPECT_EQ(r.segments[1].label, QStringLiteral("Shelf"));
    EXPECT_EQ(r.segments[2].label, QStringLiteral("Rack 1"));
    EXPECT_EQ(r.segments[3].kind, Kind::Box);
    EXPECT_EQ(r.segments[3].label, QStringLiteral("Box 7"));
    EXPECT_EQ(r.segments[4].kind, Kind::Position);
    EXPECT_EQ(r.segments[4].label, QStringLiteral("A1"));
  }

  TEST_F(LocationPathResolverTest, ResolvesSingleLevelBox) {
    // Box sits directly in the freezer's root container.
    fmgr::v1::Freezer f;
    f.set_id("frz-1");
    f.set_lab_id("lab-1");
    f.set_name("Freezer A");
    f.set_layout_root_id("root-1");
    service_.freezers.push_back(f);

    service_.roots.push_back(
        makeContainer("root-1", std::nullopt, fmgr::v1::CONTAINER_KIND_GENERIC, "Root"));
    service_.boxes["box-1"] = makeBox("box-1", "root-1", "Box 1");

    const auto r = resolve("box-1", "B3");
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(r.placed);
    EXPECT_FALSE(r.partial);
    ASSERT_EQ(r.segments.size(), 4);
    EXPECT_EQ(r.segments[0].kind, Kind::Freezer);
    EXPECT_EQ(r.segments[1].label, QStringLiteral("Root"));
    EXPECT_EQ(r.segments[2].kind, Kind::Box);
    EXPECT_EQ(r.segments[3].label, QStringLiteral("B3"));
  }

  TEST_F(LocationPathResolverTest, ReturnsEmptyForUnplacedSample) {
    const auto r = resolve("", "");
    EXPECT_TRUE(r.ok);
    EXPECT_FALSE(r.placed);
    EXPECT_TRUE(r.segments.isEmpty());
  }

  TEST_F(LocationPathResolverTest, HandlesMissingBox) {
    // box_id resolves to nothing → hard error, no crash, no segments.
    const auto r = resolve("ghost-box", "A1");
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error.empty());
    EXPECT_TRUE(r.segments.isEmpty());
  }

  TEST_F(LocationPathResolverTest, HandlesBrokenContainerChain) {
    // Rack is reachable from the root listing but its parent_id points at a
    // container that does not exist → partial path, no freezer, no infinite loop.
    service_.roots.push_back(
        makeContainer("shelf-1", std::nullopt, fmgr::v1::CONTAINER_KIND_SHELF, "Shelf"));
    service_.children["shelf-1"].push_back(
        makeContainer("rack-1", "ghost-parent", fmgr::v1::CONTAINER_KIND_RACK, "Rack"));
    service_.boxes["box-1"] = makeBox("box-1", "rack-1", "Box 7");

    const auto r = resolve("box-1", "A1");
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(r.placed);
    EXPECT_TRUE(r.partial);
    // Rack + Box + Position, but no Freezer segment (root never reached).
    ASSERT_FALSE(r.segments.isEmpty());
    EXPECT_NE(r.segments.front().kind, Kind::Freezer);
    EXPECT_EQ(r.segments.back().kind, Kind::Position);
  }

  TEST_F(LocationPathResolverTest, DetectsCycle) {
    // Self-loop: a container whose parent_id points to itself.
    // The BFS skip-guard deduplicates cross-node cycles (A→B→A) because
    // each container is visited once. Self-loops are the only cycle shape
    // that survives BFS dedup.
    auto self = makeContainer("self", "self",
                              fmgr::v1::CONTAINER_KIND_GENERIC, "Loop");
    self.set_parent_id("self");
    service_.roots.push_back(self);
    service_.boxes["box-1"] = makeBox("box-1", "self", "Looped Box");

    const auto r = resolve("box-1", "A1");
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(r.placed);
    EXPECT_TRUE(r.partial);
  }

  TEST_F(LocationPathResolverTest, EmptyPositionLabelOmitsPositionSegment) {
    fmgr::v1::Freezer f;
    f.set_id("frz-1");
    f.set_lab_id("lab-1");
    f.set_name("Freezer A");
    f.set_layout_root_id("root-1");
    service_.freezers.push_back(f);

    service_.roots.push_back(makeContainer("root-1", std::nullopt,
        fmgr::v1::CONTAINER_KIND_GENERIC, "Root"));
    service_.boxes["box-1"] = makeBox("box-1", "root-1", "Box 1");

    const auto r = resolve("box-1", "");
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(r.placed);
    EXPECT_FALSE(r.partial);
    // Should end with Box, not Position.
    ASSERT_FALSE(r.segments.isEmpty());
    EXPECT_EQ(r.segments.back().kind, Kind::Box);
  }

  TEST_F(LocationPathResolverTest, ContainerLabelPreferredOverName) {
    // containerLabel() prefers c.label over c.name.
    fmgr::v1::Freezer f;
    f.set_id("frz-1");
    f.set_lab_id("lab-1");
    f.set_name("Freezer A");
    f.set_layout_root_id("root-1");
    service_.freezers.push_back(f);

    auto root = makeContainer("root-1", std::nullopt,
                              fmgr::v1::CONTAINER_KIND_GENERIC, "SystemName");
    root.set_label("DisplayLabel");
    service_.roots.push_back(root);
    service_.boxes["box-1"] = makeBox("box-1", "root-1", "Box 1");

    const auto r = resolve("box-1", "A1");
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(r.placed);
    // Root should show "DisplayLabel" (the label), not "SystemName".
    auto it = std::find_if(r.segments.begin(), r.segments.end(),
        [](const auto& s) { return s.kind == Kind::Container; });
    ASSERT_NE(it, r.segments.end());
    EXPECT_EQ(it->label, QStringLiteral("DisplayLabel"));
  }

  TEST_F(LocationPathResolverTest, NoFreezerMatchesRootOmitsFreezerSegment) {
    // The container chain reaches a root, but no freezer's layout_root_id
    // matches it. The Freezer segment should be omitted (not an error).
    service_.roots.push_back(
        makeContainer("orphan-root", std::nullopt,
                      fmgr::v1::CONTAINER_KIND_GENERIC, "Orphan"));
    service_.boxes["box-1"] = makeBox("box-1", "orphan-root", "Box 1");

    const auto r = resolve("box-1", "A1");
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(r.placed);
    EXPECT_FALSE(r.partial);
    // No Freezer segment — the first segment should be the Container.
    ASSERT_FALSE(r.segments.isEmpty());
    EXPECT_NE(r.segments.front().kind, Kind::Freezer);
  }

  TEST_F(LocationPathResolverTest, DeepChainResolvesAllLevels) {
    // Freezer F → Compartment → Shelf → Rack → Drawer → Box [A1].
    fmgr::v1::Freezer f;
    f.set_id("frz-1");
    f.set_lab_id("lab-1");
    f.set_name("Freezer A");
    f.set_layout_root_id("comp-1");
    service_.freezers.push_back(f);

    service_.roots.push_back(
        makeContainer("comp-1", std::nullopt, fmgr::v1::CONTAINER_KIND_TOWER, "Comp"));
    service_.children["comp-1"].push_back(
        makeContainer("shelf-1", "comp-1", fmgr::v1::CONTAINER_KIND_SHELF, "S1"));
    service_.children["shelf-1"].push_back(
        makeContainer("rack-1", "shelf-1", fmgr::v1::CONTAINER_KIND_RACK, "R1"));
    service_.children["rack-1"].push_back(
        makeContainer("drawer-1", "rack-1", fmgr::v1::CONTAINER_KIND_DRAWER, "D1"));
    service_.boxes["box-1"] = makeBox("box-1", "drawer-1", "Deep Box");

    const auto r = resolve("box-1", "A1");
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(r.placed);
    EXPECT_FALSE(r.partial);
    // Freezer + 4 containers + Box + Position = 7 segments.
    EXPECT_EQ(r.segments.size(), 7);
  }

  TEST_F(LocationPathResolverTest, ListFreezersGrpcFailureSilentlyOmitsFreezer) {
    // No freezers registered at all — the listFreezers call returns an empty
    // list. The code never checks freezers.ok, so it silently iterates over
    // zero freezers and no Freezer segment is produced.
    service_.roots.push_back(
        makeContainer("root-1", std::nullopt, fmgr::v1::CONTAINER_KIND_GENERIC, "Root"));
    service_.boxes["box-1"] = makeBox("box-1", "root-1", "Box 1");

    const auto r = resolve("box-1", "A1");
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(r.placed);
    // Freezer segment absent — no freezers to match.
    ASSERT_FALSE(r.segments.isEmpty());
    EXPECT_NE(r.segments.front().kind, Kind::Freezer);
  }

} // namespace
