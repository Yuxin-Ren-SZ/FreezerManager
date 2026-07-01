// SPDX-License-Identifier: AGPL-3.0-or-later

#include "qt/AuthServiceClient.h"

#include <memory>
#include <string>

#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <gtest/gtest.h>

#include "fmgr/v1/auth.grpc.pb.h"

namespace {

using fmgr::qt::AuthServiceClient;

// In-process fake AuthService: records what the client sent and replies with
// scripted responses/statuses so the wrapper's request/response mapping and
// bearer-metadata handling can be tested without a real server.
class FakeAuthService final : public fmgr::v1::AuthService::Service {
 public:
  // Scripted behaviour.
  bool fail_login = false;
  bool reply_mfa_required = false;
  bool fail_mfa = false;
  bool fail_logout = false;

  // Captured inputs.
  std::string seen_email;
  std::string seen_password;
  std::string seen_totp;
  std::string seen_login_authorization;   // should be empty (unauthenticated)
  std::string seen_mfa_authorization;
  std::string seen_logout_authorization;

  grpc::Status Login(grpc::ServerContext* ctx,
                     const fmgr::v1::LoginRequest* req,
                     fmgr::v1::LoginResponse* resp) override {
    seen_email = req->email();
    seen_password = req->password();
    seen_login_authorization = metadata(ctx, "authorization");
    if (fail_login) {
      return {grpc::StatusCode::UNAUTHENTICATED, "bad credentials"};
    }
    resp->set_session_token("tok-123");
    resp->set_session_id("sess-1");
    resp->set_user_id("user-1");
    resp->set_mfa_required(reply_mfa_required);
    return grpc::Status::OK;
  }

  grpc::Status SubmitMfa(grpc::ServerContext* ctx,
                         const fmgr::v1::SubmitMfaRequest* req,
                         fmgr::v1::SubmitMfaResponse* /*resp*/) override {
    seen_totp = req->totp_code();
    seen_mfa_authorization = metadata(ctx, "authorization");
    if (fail_mfa) {
      return {grpc::StatusCode::PERMISSION_DENIED, "bad code"};
    }
    return grpc::Status::OK;
  }

  grpc::Status Logout(grpc::ServerContext* ctx,
                      const fmgr::v1::LogoutRequest* /*req*/,
                      fmgr::v1::LogoutResponse* /*resp*/) override {
    if (fail_logout) {
      return {grpc::StatusCode::UNAUTHENTICATED, "bad token"};
    }
    seen_logout_authorization = metadata(ctx, "authorization");
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

// Test fixture wiring the fake service to an in-process channel.
class AuthServiceClientTest : public ::testing::Test {
 protected:
  void SetUp() override {
    grpc::ServerBuilder builder;
    builder.RegisterService(&service_);
    server_ = builder.BuildAndStart();
    ASSERT_NE(server_, nullptr);
    auto channel = server_->InProcessChannel(grpc::ChannelArguments());
    client_ = std::make_unique<AuthServiceClient>(
        fmgr::v1::AuthService::NewStub(channel));
  }

  void TearDown() override {
    if (server_) {
      server_->Shutdown();
      server_->Wait();
    }
  }

  FakeAuthService service_;
  std::unique_ptr<grpc::Server> server_;
  std::unique_ptr<AuthServiceClient> client_;
};

TEST_F(AuthServiceClientTest, LoginSendsCredentialsAndMapsResponse) {
  auto result = client_->login(QStringLiteral("a@b.org"), QStringLiteral("pw"));

  EXPECT_EQ(service_.seen_email, "a@b.org");
  EXPECT_EQ(service_.seen_password, "pw");
  ASSERT_TRUE(result.ok);
  EXPECT_EQ(result.session_token, QStringLiteral("tok-123"));
  EXPECT_EQ(result.session_id, QStringLiteral("sess-1"));
  EXPECT_EQ(result.user_id, QStringLiteral("user-1"));
  EXPECT_FALSE(result.mfa_required);
}

TEST_F(AuthServiceClientTest, LoginIsUnauthenticated) {
  client_->login(QStringLiteral("a@b.org"), QStringLiteral("pw"));
  EXPECT_TRUE(service_.seen_login_authorization.empty());
}

TEST_F(AuthServiceClientTest, LoginReportsMfaRequired) {
  service_.reply_mfa_required = true;
  auto result = client_->login(QStringLiteral("a@b.org"), QStringLiteral("pw"));
  ASSERT_TRUE(result.ok);
  EXPECT_TRUE(result.mfa_required);
}

TEST_F(AuthServiceClientTest, LoginFailureCarriesError) {
  service_.fail_login = true;
  auto result = client_->login(QStringLiteral("a@b.org"), QStringLiteral("x"));
  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.error, "bad credentials");
  EXPECT_TRUE(result.session_token.isEmpty());
}

TEST_F(AuthServiceClientTest, SubmitMfaSendsCodeWithBearer) {
  auto result =
      client_->submitMfa(QStringLiteral("tok-123"), QStringLiteral("654321"));
  ASSERT_TRUE(result.ok);
  EXPECT_EQ(service_.seen_totp, "654321");
  EXPECT_EQ(service_.seen_mfa_authorization, "Bearer tok-123");
}

TEST_F(AuthServiceClientTest, SubmitMfaFailureCarriesError) {
  service_.fail_mfa = true;
  auto result =
      client_->submitMfa(QStringLiteral("tok-123"), QStringLiteral("000000"));
  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.error, "bad code");
}

TEST_F(AuthServiceClientTest, LogoutSendsBearer) {
  auto result = client_->logout(QStringLiteral("tok-123"));
  ASSERT_TRUE(result.ok);
  EXPECT_EQ(service_.seen_logout_authorization, "Bearer tok-123");
}

TEST_F(AuthServiceClientTest, LogoutSurfacesGrpcError) {
  service_.fail_logout = true;
  auto result = client_->logout(QStringLiteral("tok"));
  EXPECT_FALSE(result.ok);
  EXPECT_NE(result.error.find("bad token"), std::string::npos);
}

}  // namespace
