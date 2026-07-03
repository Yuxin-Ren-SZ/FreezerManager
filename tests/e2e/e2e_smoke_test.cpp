// SPDX-License-Identifier: AGPL-3.0-or-later
//
// End-to-end smoke tests that exercise the full stack from gRPC client to
// SQLite-backed server. These validate the most critical user journeys work
// end-to-end and catch regressions in service wiring.

#include "auth/LocalAuthProvider.h"
#include "core/box.h"
#include "core/identity.h"
#include "core/item_type.h"
#include "core/role.h"
#include "core/sample.h"
#include "server/FreezerServer.h"
#include "storage/IdentityTraits.h"
#include "storage/ItemTypeTraits.h"
#include "storage/RoleTraits.h"
#include "storage/SessionTraits.h"
#include "storage/sqlite/AuditRepositories.h"
#include "storage/sqlite/BoxGeometryRepositories.h"
#include "storage/sqlite/IdentityRepositories.h"
#include "storage/sqlite/ItemTypeRepositories.h"
#include "storage/sqlite/LayoutRepositories.h"
#include "storage/sqlite/LoginAttemptRepositories.h"
#include "storage/sqlite/RoleRepositories.h"
#include "storage/sqlite/SampleRepositories.h"
#include "storage/sqlite/SessionRepositories.h"
#include "storage/sqlite/ShareRequestRepositories.h"
#include "storage/sqlite/SqliteBackend.h"

#include <fmgr/v1/auth.grpc.pb.h>
#include <fmgr/v1/sample.grpc.pb.h>
#include <fmgr/v1/session.grpc.pb.h>
#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <memory>
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
             ("fmgr-e2e-" + std::to_string(counter.fetch_add(1)) + ".db");
    }

    // Fixture that spins up an in-process FreezerServer with a seeded admin user.
    class E2ESmokeTest : public ::testing::Test {
    protected:
      void SetUp() override {
        db_path_ = unique_db_path();
        remove_sqlite_files(db_path_);

        backend_ = std::make_unique<storage::SqliteBackend>(
            storage::SqliteBackendOptions{.database_path = db_path_.string()});
        register_all_repositories(*backend_);
        backend_->migrate_to_latest();

        provider_ = std::make_unique<auth::LocalAuthProvider>(*backend_, fast_config());
        seed_admin_user();
        seed_item_type();

        server_opts_.listen_address = "localhost:0";
        server_ = std::make_unique<server::FreezerServer>(*backend_, *provider_, server_opts_);
        server_->build();

        server_thread_ = std::thread([this] { server_->wait(); });

        const std::string addr = "localhost:" + std::to_string(server_->bound_port());
        channel_ = grpc::CreateChannel(addr, grpc::InsecureChannelCredentials());
        auth_stub_ = fmgr::v1::AuthService::NewStub(channel_);
        session_stub_ = fmgr::v1::SessionService::NewStub(channel_);
        sample_stub_ = fmgr::v1::SampleService::NewStub(channel_);
      }

      void TearDown() override {
        if (server_) {
          server_->shutdown();
        }
        if (server_thread_.joinable()) {
          server_thread_.join();
        }
        server_.reset();
        provider_.reset();
        backend_.reset();
        remove_sqlite_files(db_path_);
      }

      // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
      [[nodiscard]] std::string login(const std::string& email, const std::string& password) {
        grpc::ClientContext ctx;
        fmgr::v1::LoginRequest req;
        req.set_email(email);
        req.set_password(password);
        fmgr::v1::LoginResponse resp;
        const auto status = auth_stub_->Login(&ctx, req, &resp);
        return status.ok() ? resp.session_token() : "";
      }

      static void set_bearer(grpc::ClientContext& ctx, const std::string& token) {
        ctx.AddMetadata("authorization", "Bearer " + token);
      }

      const std::string kEmail{"admin@e2e-test.local"};
      const std::string kPassword{"e2e-password"};

      std::filesystem::path db_path_;
      std::unique_ptr<storage::SqliteBackend> backend_;
      std::unique_ptr<auth::LocalAuthProvider> provider_;
      server::FreezerServerOptions server_opts_;
      std::unique_ptr<server::FreezerServer> server_;
      std::thread server_thread_;
      std::shared_ptr<grpc::Channel> channel_;
      std::unique_ptr<fmgr::v1::AuthService::Stub> auth_stub_;
      std::unique_ptr<fmgr::v1::SessionService::Stub> session_stub_;
      std::unique_ptr<fmgr::v1::SampleService::Stub> sample_stub_;
      core::ItemTypeId item_type_id_;

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
        storage::register_login_attempt_repositories(b);
        storage::register_audit_repositories(b);
        storage::register_box_geometry_repositories(b);
        storage::register_box_repositories(b);
        storage::register_item_type_repositories(b);
        storage::register_layout_repositories(b);
        storage::register_sample_repositories(b);
        storage::register_share_request_repositories(b);
      }

      void seed_admin_user() {
        const auto password_hash = provider_->hash_password(kPassword);
        const core::UserId uid = core::UserId::parse("10000000-e2e0-4e2e-8e2e-000000000001");
        const core::LabId lab_id = core::LabId::parse("20000000-e2e0-4e2e-8e2e-000000000001");
        const core::User user{
            .id = uid,
            .primary_email = kEmail,
            .display_name = "E2E Admin",
            .status = core::UserStatus::Active,
            .created_at = core::Timestamp::from_unix_micros(1),
            .auth_bindings = nlohmann::json::array({
                nlohmann::json::object({{"provider", "local"}, {"hash", password_hash}}),
            }),
        };
        const core::Lab lab{
            .id = lab_id,
            .name = "E2E Lab",
            .contact = "e2e@example.com",
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
            .actor_session_id = "e2e-seed",
            .request_id = "e2e-seed",
            .reason = "e2e test setup",
        };
        auto txn = backend_->begin(storage::IsolationLevel::Serializable);
        txn->repo<core::Lab>().insert(lab, ctx);
        txn->repo<core::User>().insert(user, ctx);
        txn->repo<core::LabMembership>().insert(membership, ctx);
        txn->commit();
      }

      void seed_item_type() {
        item_type_id_ = core::ItemTypeId::parse("30000000-e2e0-4e2e-8e2e-000000000001");
        const core::LabId lab_id = core::LabId::parse("20000000-e2e0-4e2e-8e2e-000000000001");
        const core::ItemType item_type{
            .id = item_type_id_,
            .lab_id = lab_id,
            .parent_id = std::nullopt,
            .name = "E2E Item Type",
            .created_at = core::Timestamp::from_unix_micros(1),
        };
        const storage::MutationContext ctx{
            .actor_user_id = core::UserId::parse("10000000-e2e0-4e2e-8e2e-000000000001"),
            .actor_session_id = "e2e-seed",
            .request_id = "e2e-seed",
            .reason = "e2e test setup",
        };
        auto txn = backend_->begin(storage::IsolationLevel::Serializable);
        txn->repo<core::ItemType>().insert(item_type, ctx);
        txn->commit();
      }
    };

    // ---- E2E smoke tests ----

    TEST_F(E2ESmokeTest, FullAuthFlowLoginLogout) {
      const auto token = login(kEmail, kPassword);
      ASSERT_FALSE(token.empty()) << "login failed";

      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::LogoutRequest req;
      fmgr::v1::LogoutResponse resp;
      const auto status = auth_stub_->Logout(&ctx, req, &resp);
      EXPECT_TRUE(status.ok()) << "logout failed: " << status.error_message();
    }

    TEST_F(E2ESmokeTest, ListSessionsAfterLogin) {
      const auto token = login(kEmail, kPassword);
      ASSERT_FALSE(token.empty());

      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::ListSessionsRequest req;
      fmgr::v1::ListSessionsResponse resp;
      const auto status = session_stub_->ListSessions(&ctx, req, &resp);
      EXPECT_TRUE(status.ok()) << status.error_message();
    }

    TEST_F(E2ESmokeTest, ListSamplesAsAuthenticatedUser) {
      const auto token = login(kEmail, kPassword);
      ASSERT_FALSE(token.empty());

      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::ListSamplesRequest req;
      req.set_lab_id("20000000-e2e0-4e2e-8e2e-000000000001");
      fmgr::v1::ListSamplesResponse resp;
      const auto status = sample_stub_->ListSamples(&ctx, req, &resp);
      EXPECT_TRUE(status.ok()) << status.error_message();
      // Lab is empty — list should succeed with zero samples.
    }

    TEST_F(E2ESmokeTest, UnauthenticatedRequestIsRejected) {
      grpc::ClientContext ctx;
      fmgr::v1::ListSamplesRequest req;
      fmgr::v1::ListSamplesResponse resp;
      const auto status = sample_stub_->ListSamples(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::UNAUTHENTICATED);
    }

    TEST_F(E2ESmokeTest, SampleCreateAndListLifecycle) {
      const auto token = login(kEmail, kPassword);
      ASSERT_FALSE(token.empty());

      // Create a sample via the gRPC API.
      grpc::ClientContext create_ctx;
      set_bearer(create_ctx, token);
      fmgr::v1::CreateSampleRequest create_req;
      create_req.set_lab_id("20000000-e2e0-4e2e-8e2e-000000000001");
      create_req.set_item_type_id(item_type_id_.to_string());
      create_req.set_name("E2E Created Sample");
      fmgr::v1::CreateSampleResponse create_resp;
      const auto create_status = sample_stub_->CreateSample(&create_ctx, create_req, &create_resp);
      ASSERT_TRUE(create_status.ok()) << "CreateSample failed: " << create_status.error_message();
      EXPECT_FALSE(create_resp.sample().id().empty());
      EXPECT_EQ(create_resp.sample().name(), "E2E Created Sample");
      const std::string created_id = create_resp.sample().id();

      // List samples — the newly created sample must appear.
      grpc::ClientContext list_ctx;
      set_bearer(list_ctx, token);
      fmgr::v1::ListSamplesRequest list_req;
      list_req.set_lab_id("20000000-e2e0-4e2e-8e2e-000000000001");
      fmgr::v1::ListSamplesResponse list_resp;
      const auto list_status = sample_stub_->ListSamples(&list_ctx, list_req, &list_resp);
      ASSERT_TRUE(list_status.ok()) << "ListSamples failed: " << list_status.error_message();
      EXPECT_EQ(list_resp.samples_size(), 1);

      bool found = false;
      for (int i = 0; i < list_resp.samples_size(); ++i) {
        if (list_resp.samples(i).id() == created_id) {
          found = true;
          EXPECT_EQ(list_resp.samples(i).name(), "E2E Created Sample");
          EXPECT_EQ(list_resp.samples(i).status(), fmgr::v1::SAMPLE_STATUS_ACTIVE);
        }
      }
      EXPECT_TRUE(found) << "created sample not found in list response";
    }

  } // namespace
} // namespace fmgr::test
