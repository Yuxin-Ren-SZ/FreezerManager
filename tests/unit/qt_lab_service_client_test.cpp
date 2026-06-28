// SPDX-License-Identifier: AGPL-3.0-or-later

#include "qt/LabServiceClient.h"

#include <memory>
#include <string>

#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <gtest/gtest.h>

#include "fmgr/v1/lab.grpc.pb.h"

namespace {

using fmgr::qt::LabServiceClient;

// In-process fake LabService: records the bearer the client sent and replies
// with scripted labs, so the wrapper's request/response mapping can be tested
// without a real server. Mirrors FakeAuthService in qt_auth_service_client_test.
class FakeLabService final : public fmgr::v1::LabService::Service {
 public:
  bool fail_list = false;
  std::string seen_list_authorization;
  std::string seen_get_lab_id;
  std::string seen_get_authorization;

  grpc::Status ListLabs(grpc::ServerContext* ctx,
                        const fmgr::v1::ListLabsRequest* /*req*/,
                        fmgr::v1::ListLabsResponse* resp) override {
    seen_list_authorization = metadata(ctx, "authorization");
    if (fail_list) {
      return {grpc::StatusCode::PERMISSION_DENIED, "nope"};
    }
    fmgr::v1::Lab* a = resp->add_labs();
    a->set_id("lab-1");
    a->set_name("Alpha Lab");
    a->set_contact("alpha@uni.edu");
    a->set_is_phi_enabled(true);
    fmgr::v1::Lab* b = resp->add_labs();
    b->set_id("lab-2");
    b->set_name("Beta Lab");
    return grpc::Status::OK;
  }

  grpc::Status GetLab(grpc::ServerContext* ctx,
                      const fmgr::v1::GetLabRequest* req,
                      fmgr::v1::GetLabResponse* resp) override {
    seen_get_lab_id = req->lab_id();
    seen_get_authorization = metadata(ctx, "authorization");
    fmgr::v1::Lab* lab = resp->mutable_lab();
    lab->set_id(req->lab_id());
    lab->set_name("Resolved Lab");
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

class LabServiceClientTest : public ::testing::Test {
 protected:
  void SetUp() override {
    grpc::ServerBuilder builder;
    builder.RegisterService(&service_);
    server_ = builder.BuildAndStart();
    ASSERT_NE(server_, nullptr);
    auto channel = server_->InProcessChannel(grpc::ChannelArguments());
    client_ = std::make_unique<LabServiceClient>(
        fmgr::v1::LabService::NewStub(channel));
  }

  void TearDown() override {
    if (server_) {
      server_->Shutdown();
      server_->Wait();
    }
  }

  FakeLabService service_;
  std::unique_ptr<grpc::Server> server_;
  std::unique_ptr<LabServiceClient> client_;
};

TEST_F(LabServiceClientTest, ListLabsMapsRowsAndSendsBearer) {
  auto result = client_->listLabs(QStringLiteral("tok-123"));

  ASSERT_TRUE(result.ok);
  EXPECT_EQ(service_.seen_list_authorization, "Bearer tok-123");
  ASSERT_EQ(result.labs.size(), 2u);
  EXPECT_EQ(result.labs[0].id, QStringLiteral("lab-1"));
  EXPECT_EQ(result.labs[0].name, QStringLiteral("Alpha Lab"));
  EXPECT_EQ(result.labs[0].contact, QStringLiteral("alpha@uni.edu"));
  EXPECT_TRUE(result.labs[0].is_phi_enabled);
  EXPECT_EQ(result.labs[1].id, QStringLiteral("lab-2"));
  EXPECT_FALSE(result.labs[1].is_phi_enabled);
}

TEST_F(LabServiceClientTest, ListLabsFailureCarriesError) {
  service_.fail_list = true;
  auto result = client_->listLabs(QStringLiteral("tok-123"));
  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.error, "nope");
  EXPECT_TRUE(result.labs.empty());
}

TEST_F(LabServiceClientTest, GetLabPassesIdAndBearer) {
  auto result = client_->getLab(QStringLiteral("tok-9"), QStringLiteral("lab-7"));
  ASSERT_TRUE(result.ok);
  EXPECT_EQ(service_.seen_get_lab_id, "lab-7");
  EXPECT_EQ(service_.seen_get_authorization, "Bearer tok-9");
  EXPECT_EQ(result.lab.id, QStringLiteral("lab-7"));
  EXPECT_EQ(result.lab.name, QStringLiteral("Resolved Lab"));
}

}  // namespace
