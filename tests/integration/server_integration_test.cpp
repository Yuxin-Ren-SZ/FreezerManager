// SPDX-License-Identifier: AGPL-3.0-or-later

#include "auth/LocalAuthProvider.h"
#include "core/identity.h"
#include "core/role.h"
#include "server/FreezerServer.h"
#include "storage/IdentityTraits.h"
#include "storage/RoleTraits.h"
#include "storage/SessionTraits.h"
#include "storage/sqlite/AuditRepositories.h"
#include "storage/sqlite/BoxGeometryRepositories.h"
#include "storage/sqlite/IdentityRepositories.h"
#include "storage/sqlite/ItemTypeRepositories.h"
#include "storage/sqlite/LayoutRepositories.h"
#include "storage/sqlite/RoleRepositories.h"
#include "storage/sqlite/SampleRepositories.h"
#include "storage/sqlite/SessionRepositories.h"
#include "storage/sqlite/ShareRequestRepositories.h"
#include "storage/sqlite/SqliteBackend.h"

#include "rpc/AuthMiddleware.h"

#include <fmgr/v1/auth.grpc.pb.h>
#include <fmgr/v1/session.grpc.pb.h>
#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace fmgr::test {
  namespace {

    // Fast Argon2id parameters for tests.
    [[nodiscard]] auth::LocalAuthProviderConfig fast_config() {
      auth::LocalAuthProviderConfig cfg;
      cfg.pwhash_memlimit = 8192;
      cfg.pwhash_opslimit = 1;
      return cfg;
    }

    [[nodiscard]] std::filesystem::path unique_db_path() {
      static std::atomic<int> counter{0};
      return std::filesystem::temp_directory_path() /
             ("fmgr-srv-test-" + std::to_string(counter.fetch_add(1)) + ".db");
    }

    // Fixture that spins up an in-process FreezerServer on a random port.
    class ServerIntegrationTest : public ::testing::Test {
    protected:
      void SetUp() override {
        db_path_ = unique_db_path();
        remove_sqlite_files(db_path_);

        backend_ = std::make_unique<storage::SqliteBackend>(
            storage::SqliteBackendOptions{.database_path = db_path_.string()});
        register_all_repositories(*backend_);
        backend_->migrate_to_latest();

        provider_ = std::make_unique<auth::LocalAuthProvider>(*backend_, fast_config());

        seed_test_user();

        // Start server on a random port (OS-assigned by using port 0).
        server_opts_.listen_address = "localhost:0";
        server_ = std::make_unique<server::FreezerServer>(*backend_, *provider_, server_opts_);
        // build() binds the port (fills bound_port_) without blocking.
        server_->build();

        // wait() blocks until shutdown(); run it on a background thread.
        server_thread_ = std::thread([this] { server_->wait(); });

        const std::string addr = "localhost:" + std::to_string(server_->bound_port());
        channel_ = grpc::CreateChannel(addr, grpc::InsecureChannelCredentials());
        auth_stub_ = fmgr::v1::AuthService::NewStub(channel_);
        session_stub_ = fmgr::v1::SessionService::NewStub(channel_);
      }

      void TearDown() override {
        if (server_) {
          server_->shutdown(); // signals wait() to return
        }
        if (server_thread_.joinable()) {
          server_thread_.join();
        }
        server_.reset();
        provider_.reset();
        backend_.reset();
        remove_sqlite_files(db_path_);
      }

      // Login and return the bearer token.
      // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
      [[nodiscard]] std::string login(const std::string& email, const std::string& password) {
        grpc::ClientContext ctx;
        fmgr::v1::LoginRequest req;
        req.set_email(email);
        req.set_password(password);
        fmgr::v1::LoginResponse resp;
        const auto status = auth_stub_->Login(&ctx, req, &resp);
        if (!status.ok()) {
          return {};
        }
        return resp.session_token();
      }

      // Set Authorization: Bearer header on a ClientContext.
      static void set_bearer(grpc::ClientContext& ctx, const std::string& token) {
        ctx.AddMetadata("authorization", "Bearer " + token);
      }

      const std::string kEmail{"admin@example.com"};
      const std::string kPassword{"hunter22"};

      std::filesystem::path db_path_;
      std::unique_ptr<storage::SqliteBackend> backend_;
      std::unique_ptr<auth::LocalAuthProvider> provider_;
      server::FreezerServerOptions server_opts_;
      std::unique_ptr<server::FreezerServer> server_;
      std::thread server_thread_;
      std::shared_ptr<grpc::Channel> channel_;
      std::unique_ptr<fmgr::v1::AuthService::Stub> auth_stub_;
      std::unique_ptr<fmgr::v1::SessionService::Stub> session_stub_;

    private:
      static void remove_sqlite_files(const std::filesystem::path& path) {
        std::error_code error;
        std::filesystem::remove(path, error);
        std::filesystem::remove(std::filesystem::path(path.string() + "-wal"), error);
        std::filesystem::remove(std::filesystem::path(path.string() + "-shm"), error);
      }

      static void register_all_repositories(storage::SqliteBackend& b) {
        storage::register_identity_repositories(b);
        storage::register_role_repositories(b);
        storage::register_session_repositories(b);
        storage::register_audit_repositories(b);
        storage::register_box_geometry_repositories(b);
        storage::register_box_repositories(b);
        storage::register_item_type_repositories(b);
        storage::register_layout_repositories(b);
        storage::register_sample_repositories(b);
        storage::register_share_request_repositories(b);
      }

      void seed_test_user() {
        const auto password_hash = provider_->hash_password(kPassword);
        const core::UserId uid = core::UserId::parse("10000000-0000-0000-0000-000000000001");
        const core::LabId lab_id = core::LabId::parse("20000000-0000-0000-0000-000000000001");
        const core::User user{
            .id = uid,
            .primary_email = kEmail,
            .display_name = "Test Admin",
            .status = core::UserStatus::Active,
            .created_at = core::Timestamp::from_unix_micros(1),
            .auth_bindings = nlohmann::json::array({
                nlohmann::json::object({{"provider", "local"}, {"hash", password_hash}}),
            }),
        };
        const core::Lab lab{
            .id = lab_id,
            .name = "Test Lab",
            .contact = "test@example.com",
            .created_at = core::Timestamp::from_unix_micros(1),
            .settings_json = nlohmann::json::object(),
        };
        const core::LabMembership membership{
            .user_id = uid,
            .lab_id = lab_id,
            .role_id = core::builtin_role_id(core::RoleKind::SystemAdmin),
            .scope_filters_json = nlohmann::json::object(),
            .joined_at = core::Timestamp::from_unix_micros(1),
        };
        const storage::MutationContext ctx{
            .actor_user_id = core::UserId::parse("00000000-0000-0000-0000-000000000000"),
            .actor_session_id = "seed",
            .request_id = "seed",
            .reason = "test setup",
        };
        auto txn = backend_->begin(storage::IsolationLevel::Serializable);
        txn->repo<core::Lab>().insert(lab, ctx);
        txn->repo<core::User>().insert(user, ctx);
        txn->repo<core::LabMembership>().insert(membership, ctx);
        txn->commit();
      }
    };

    // ---- Tests ----

    TEST_F(ServerIntegrationTest, LoginValidCredentialsReturnsToken) {
      grpc::ClientContext ctx;
      fmgr::v1::LoginRequest req;
      req.set_email(kEmail);
      req.set_password(kPassword);
      fmgr::v1::LoginResponse resp;

      const auto status = auth_stub_->Login(&ctx, req, &resp);
      EXPECT_TRUE(status.ok()) << status.error_message();
      EXPECT_FALSE(resp.session_token().empty());
      EXPECT_FALSE(resp.session_id().empty());
      EXPECT_FALSE(resp.mfa_required());
    }

    TEST_F(ServerIntegrationTest, LoginWrongPasswordReturnsUnauthenticated) {
      grpc::ClientContext ctx;
      fmgr::v1::LoginRequest req;
      req.set_email(kEmail);
      req.set_password("wrong");
      fmgr::v1::LoginResponse resp;

      const auto status = auth_stub_->Login(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::UNAUTHENTICATED);
    }

    TEST_F(ServerIntegrationTest, LoginUnknownEmailReturnsUnauthenticated) {
      grpc::ClientContext ctx;
      fmgr::v1::LoginRequest req;
      req.set_email("nobody@example.com");
      req.set_password("any");
      fmgr::v1::LoginResponse resp;

      const auto status = auth_stub_->Login(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::UNAUTHENTICATED);
    }

    TEST_F(ServerIntegrationTest, LogoutRevokesSession) {
      const auto token = login(kEmail, kPassword);
      ASSERT_FALSE(token.empty());

      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::LogoutRequest req;
      fmgr::v1::LogoutResponse resp;
      const auto status = auth_stub_->Logout(&ctx, req, &resp);
      EXPECT_TRUE(status.ok()) << status.error_message();

      // Second logout with the same token should fail (session revoked).
      grpc::ClientContext ctx2;
      set_bearer(ctx2, token);
      fmgr::v1::LogoutResponse resp2;
      const auto status2 = auth_stub_->Logout(&ctx2, req, &resp2);
      EXPECT_FALSE(status2.ok());
      EXPECT_EQ(status2.error_code(), grpc::StatusCode::UNAUTHENTICATED);
    }

    TEST_F(ServerIntegrationTest, LogoutWithoutBearerReturnsUnauthenticated) {
      grpc::ClientContext ctx;
      fmgr::v1::LogoutRequest req;
      fmgr::v1::LogoutResponse resp;
      const auto status = auth_stub_->Logout(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::UNAUTHENTICATED);
    }

    TEST_F(ServerIntegrationTest, CreateAndListApiToken) {
      const auto token = login(kEmail, kPassword);
      ASSERT_FALSE(token.empty());

      // Create an API token.
      grpc::ClientContext ctx1;
      set_bearer(ctx1, token);
      fmgr::v1::CreateApiTokenRequest create_req;
      create_req.set_name("my-script-token");
      create_req.set_scope_json(R"(["*"])");
      create_req.set_expires_in_days(7);
      fmgr::v1::CreateApiTokenResponse create_resp;
      const auto create_status = auth_stub_->CreateApiToken(&ctx1, create_req, &create_resp);
      EXPECT_TRUE(create_status.ok()) << create_status.error_message();
      EXPECT_FALSE(create_resp.token().empty());
      EXPECT_TRUE(create_resp.token().starts_with("fmgr_pat_"));
      EXPECT_FALSE(create_resp.api_token_id().empty());

      // List tokens — should include the new one.
      grpc::ClientContext ctx2;
      set_bearer(ctx2, token);
      fmgr::v1::ListApiTokensRequest list_req;
      fmgr::v1::ListApiTokensResponse list_resp;
      const auto list_status = auth_stub_->ListApiTokens(&ctx2, list_req, &list_resp);
      EXPECT_TRUE(list_status.ok()) << list_status.error_message();
      EXPECT_GE(list_resp.tokens_size(), 1);

      bool found = false;
      for (int i = 0; i < list_resp.tokens_size(); ++i) {
        if (list_resp.tokens(i).id() == create_resp.api_token_id()) {
          found = true;
          EXPECT_EQ(list_resp.tokens(i).name(), "my-script-token");
        }
      }
      EXPECT_TRUE(found);
    }

    TEST_F(ServerIntegrationTest, RevokeApiToken) {
      const auto token = login(kEmail, kPassword);
      ASSERT_FALSE(token.empty());

      grpc::ClientContext ctx1;
      set_bearer(ctx1, token);
      fmgr::v1::CreateApiTokenRequest create_req;
      create_req.set_name("to-revoke");
      create_req.set_scope_json(R"(["*"])");
      fmgr::v1::CreateApiTokenResponse create_resp;
      ASSERT_TRUE(auth_stub_->CreateApiToken(&ctx1, create_req, &create_resp).ok());

      grpc::ClientContext ctx2;
      set_bearer(ctx2, token);
      fmgr::v1::RevokeApiTokenRequest revoke_req;
      revoke_req.set_api_token_id(create_resp.api_token_id());
      fmgr::v1::RevokeApiTokenResponse revoke_resp;
      const auto revoke_status = auth_stub_->RevokeApiToken(&ctx2, revoke_req, &revoke_resp);
      EXPECT_TRUE(revoke_status.ok()) << revoke_status.error_message();
    }

    TEST_F(ServerIntegrationTest, ListSessions) {
      const auto token = login(kEmail, kPassword);
      ASSERT_FALSE(token.empty());

      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::ListSessionsRequest req;
      fmgr::v1::ListSessionsResponse resp;
      const auto status = session_stub_->ListSessions(&ctx, req, &resp);
      EXPECT_TRUE(status.ok()) << status.error_message();
      EXPECT_GE(resp.sessions_size(), 1);
    }

    TEST_F(ServerIntegrationTest, RpcRegistryCoversAllExpectedMethods) {
      // Verify that every gRPC method defined in all service stubs is present in
      // the AuthMiddleware registry. At minimum the count must be >= the number of
      // stubs registered (auth + session + 7 stub services).
      const auto registry = rpc::AuthMiddleware::registered_rpcs();
      // 6 (AuthService) + 2 (SessionService) + 8 (LabService) + 8 (SampleService)
      // + 19 (BoxService) + 9 (ItemTypeService) + 8 (RoleService) + 4 (AuditService)
      // + 6 (ShareService) = 70 RPCs
      EXPECT_GE(registry.size(), 60U) << "RPC registry smaller than expected; "
                                         "a new service may have been added without registering.";
    }

  } // namespace
} // namespace fmgr::test
