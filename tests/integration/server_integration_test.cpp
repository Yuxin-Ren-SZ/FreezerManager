// SPDX-License-Identifier: AGPL-3.0-or-later

#include "auth/LocalAuthProvider.h"
#include "core/identity.h"
#include "core/permissions.h"
#include "core/role.h"
#include "server/FreezerServer.h"
#include "server/GrpcErrorTranslation.h"
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
#include <fmgr/v1/lab.grpc.pb.h>
#include <fmgr/v1/sample.grpc.pb.h>
#include <fmgr/v1/session.grpc.pb.h>
#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

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
        // Unmask INTERNAL detail: the registry-mismatch test (#60) asserts that
        // the refusal names both permissions, and the default masks it in release
        // builds (NDEBUG). Set explicitly rather than relying on the debug
        // default, so the assertion holds in every preset CI builds.
        server_opts_.mask_internal_errors = false;
        server_ = std::make_unique<server::FreezerServer>(*backend_, *provider_, server_opts_);
        // build() binds the port (fills bound_port_) without blocking.
        server_->build();

        // wait() blocks until shutdown(); run it on a background thread.
        server_thread_ = std::thread([this] { server_->wait(); });

        const std::string addr = "localhost:" + std::to_string(server_->bound_port());
        channel_ = grpc::CreateChannel(addr, grpc::InsecureChannelCredentials());
        auth_stub_ = fmgr::v1::AuthService::NewStub(channel_);
        session_stub_ = fmgr::v1::SessionService::NewStub(channel_);
        sample_stub_ = fmgr::v1::SampleService::NewStub(channel_);
        lab_stub_ = fmgr::v1::LabService::NewStub(channel_);
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
      const std::string kMfaEmail{"mfa@example.com"};
      // #62: a third account, TOTP-enrolled. `...003` because #78 took `...002`
      // for its permissionless account — the two ids collided when this branch
      // was rebased onto it, which the fixture reports as "user id already
      // exists" from every test's SetUp().
      const std::string kMfaUserId{"10000000-0000-0000-0000-000000000003"};
      const std::string kPassword{"hunter22"};
      // RFC 6238's test secret; the value only has to match what the seed writes
      // into `totp_secret_enc` for the account to count as MFA-enrolled.
      static constexpr std::string_view kTotpSecret = "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ";
      // The lab seed_test_user() creates; its id is what the layout/sample RPCs
      // are called with (#60).
      const std::string kLabId{"20000000-0000-0000-0000-000000000001"};
      // #78: a second account with no lab membership at all, so
      // resolve_permissions() grants it nothing — the caller criterion 3 of the
      // issue talks about ("authenticated but holds no permissions").
      const std::string kNoPermissionEmail{"nobody@example.com"};
      const std::string kNoPermissionUserId{"10000000-0000-0000-0000-000000000002"};
      // #140: a caller who belongs to two labs with a different role in each.
      // `...004` continues the id sequence above rather than reusing one, so a
      // collision with another fixture account is a compile-time constant clash
      // instead of an "user id already exists" failure in every test's SetUp()
      // (the #117/#118 merge hazard the board records).
      const std::string kTwoLabEmail{"two-lab@example.com"};
      const std::string kTwoLabUserId{"10000000-0000-0000-0000-000000000004"};
      const std::string kSecondLabId{"20000000-0000-0000-0000-000000000002"};
      static constexpr std::string_view kSecondLabScopeFilter =
          R"({"freezer_in":["30000000-0000-0000-0000-000000000001"]})";

      // #140: seed the two-lab caller. Separate from seed_test_user() so no other
      // test's expectations about the shared lab change; every row here belongs
      // to the account nothing else logs in as.
      void seed_two_lab_caller() {
        const auto password_hash = provider_->hash_password(kPassword);
        const core::User user{
            .id = core::UserId::parse(kTwoLabUserId),
            .primary_email = kTwoLabEmail,
            .display_name = "Two Lab User",
            .status = core::UserStatus::Active,
            .created_at = core::Timestamp::from_unix_micros(1),
            .auth_bindings = nlohmann::json::array(
                {nlohmann::json::object({{"provider", "local"}, {"hash", password_hash}})}),
        };
        // PHI enabled on the *second* lab only, so a response that defaulted the
        // flag — or read the first lab's — fails the assertion that reads it back.
        const core::Lab second_lab{
            .id = core::LabId::parse(kSecondLabId),
            .name = "Second Lab",
            .contact = "second@example.com",
            .created_at = core::Timestamp::from_unix_micros(1),
            .settings_json = nlohmann::json::object(),
            .is_phi_enabled = true,
        };
        const core::LabMembership admin_of_the_first{
            .user_id = user.id,
            .lab_id = core::LabId::parse(kLabId),
            .role_id = core::builtin_role_id(core::RoleKind::LabAdmin),
            .scope_filters_json = nlohmann::json::object(),
            .joined_at = core::Timestamp::from_unix_micros(1),
        };
        const core::LabMembership member_of_the_second{
            .user_id = user.id,
            .lab_id = second_lab.id,
            .role_id = core::builtin_role_id(core::RoleKind::Member),
            .scope_filters_json = nlohmann::json::parse(kSecondLabScopeFilter),
            .joined_at = core::Timestamp::from_unix_micros(1),
        };
        const storage::MutationContext ctx{
            .actor_user_id = core::UserId::parse("00000000-0000-0000-0000-000000000000"),
            .actor_session_id = "seed",
            .request_id = "seed",
            .reason = "test setup",
        };
        auto txn = backend_->begin(storage::IsolationLevel::Serializable);
        txn->repo<core::User>().insert(user, ctx);
        txn->repo<core::Lab>().insert(second_lab, ctx);
        txn->repo<core::LabMembership>().insert(admin_of_the_first, ctx);
        txn->repo<core::LabMembership>().insert(member_of_the_second, ctx);
        txn->commit();
      }

      // The membership entry for `lab_id`, or nullptr. The response's lab order
      // is an implementation detail, so assertions look entries up by id.
      [[nodiscard]] static const fmgr::v1::WhoAmIMembership*
      lab_entry(const fmgr::v1::WhoAmIResponse& resp, const std::string& lab_id) {
        for (const auto& lab : resp.labs()) {
          if (lab.lab_id() == lab_id) {
            return &lab;
          }
        }
        return nullptr;
      }

      [[nodiscard]] static bool holds(const fmgr::v1::WhoAmIMembership& lab,
                                      std::string_view permission) {
        return std::find(lab.permissions().begin(), lab.permissions().end(), permission) !=
               lab.permissions().end();
      }

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
      std::unique_ptr<fmgr::v1::LabService::Stub> lab_stub_;

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
        const core::LabId lab_id = core::LabId::parse(kLabId);
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
        // A second account with TOTP enrolled: its sessions start with
        // mfa_complete=false, which is what a browser holds after the login route
        // has already set the cookie (#62).
        const core::User mfa_user{
            .id = core::UserId::parse(kMfaUserId),
            .primary_email = kMfaEmail,
            .display_name = "Test MFA User",
            .status = core::UserStatus::Active,
            .created_at = core::Timestamp::from_unix_micros(1),
            .auth_bindings = nlohmann::json::array({
                nlohmann::json::object({{"provider", "local"}, {"hash", password_hash}}),
            }),
            .totp_secret_enc = std::string(kTotpSecret),
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
        const core::LabMembership mfa_membership{
            .user_id = mfa_user.id,
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
        txn->repo<core::User>().insert(mfa_user, ctx);
        txn->repo<core::LabMembership>().insert(membership, ctx);
        txn->repo<core::LabMembership>().insert(mfa_membership, ctx);

        // #78: the permissionless account. A user with no membership resolves to
        // an empty grant set — no lab permissions, no global ones — which is the
        // caller the non-permission RPCs must still admit and the permission-gated
        // ones must still refuse.
        const core::User no_permission_user{
            .id = core::UserId::parse(kNoPermissionUserId),
            .primary_email = kNoPermissionEmail,
            .display_name = "No Role",
            .status = core::UserStatus::Active,
            .created_at = core::Timestamp::from_unix_micros(1),
            .auth_bindings = nlohmann::json::array({
                nlohmann::json::object({{"provider", "local"}, {"hash", password_hash}}),
            }),
        };
        txn->repo<core::User>().insert(no_permission_user, ctx);
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

    // A session whose second factor is still outstanding is still a credential:
    // the browser login route has already set the cookie by the time `Login`
    // answers `mfa_required`. Logout only removes authority, so it is allowed
    // for a pending session — otherwise abandoning the TOTP prompt leaves the
    // browser holding a cookie nothing can revoke (`SameSite=Strict`).
    TEST_F(ServerIntegrationTest, LogoutRevokesAPendingMfaSession) {
      grpc::ClientContext login_ctx;
      fmgr::v1::LoginRequest login_req;
      login_req.set_email(kMfaEmail);
      login_req.set_password(kPassword);
      fmgr::v1::LoginResponse login_resp;
      const auto login_status = auth_stub_->Login(&login_ctx, login_req, &login_resp);
      ASSERT_TRUE(login_status.ok()) << login_status.error_message();
      ASSERT_TRUE(login_resp.mfa_required());
      const auto& token = login_resp.session_token();
      ASSERT_FALSE(token.empty());

      grpc::ClientContext logout_ctx;
      set_bearer(logout_ctx, token);
      fmgr::v1::LogoutRequest logout_req;
      fmgr::v1::LogoutResponse logout_resp;
      const auto logout_status = auth_stub_->Logout(&logout_ctx, logout_req, &logout_resp);
      EXPECT_TRUE(logout_status.ok()) << logout_status.error_message();

      // Revoked server-side, not merely answered OK: the credential is gone, and
      // it is gone as an invalid token rather than as a still-pending session.
      // The error message is the whole distinction, so assert on it.
      grpc::ClientContext after_ctx;
      set_bearer(after_ctx, token);
      fmgr::v1::ListApiTokensRequest list_req;
      fmgr::v1::ListApiTokensResponse list_resp;
      const auto after_status = auth_stub_->ListApiTokens(&after_ctx, list_req, &list_resp);
      ASSERT_FALSE(after_status.ok());
      EXPECT_EQ(after_status.error_code(), grpc::StatusCode::UNAUTHENTICATED);
      EXPECT_EQ(after_status.error_message().find("mfa_required"), std::string::npos)
          << after_status.error_message();
    }

    // Logout is the de-escalation, not a general widening: every other
    // self-management RPC still refuses a session whose TOTP is outstanding, and
    // SubmitMfa is the only other RPC that reaches its handler without the gate.
    TEST_F(ServerIntegrationTest, PendingMfaSessionIsStillRefusedByTheOtherSelfManagementRpcs) {
      const auto token = login(kMfaEmail, kPassword);
      ASSERT_FALSE(token.empty());

      grpc::ClientContext create_ctx;
      set_bearer(create_ctx, token);
      fmgr::v1::CreateApiTokenRequest create_req;
      create_req.set_name("must-not-exist");
      create_req.set_scope_json(R"(["*"])");
      fmgr::v1::CreateApiTokenResponse create_resp;
      const auto create_status = auth_stub_->CreateApiToken(&create_ctx, create_req, &create_resp);
      ASSERT_FALSE(create_status.ok());
      EXPECT_EQ(create_status.error_code(), grpc::StatusCode::UNAUTHENTICATED);
      EXPECT_NE(create_status.error_message().find("mfa_required"), std::string::npos)
          << create_status.error_message();

      grpc::ClientContext list_ctx;
      set_bearer(list_ctx, token);
      fmgr::v1::ListApiTokensRequest list_req;
      fmgr::v1::ListApiTokensResponse list_resp;
      const auto list_status = auth_stub_->ListApiTokens(&list_ctx, list_req, &list_resp);
      ASSERT_FALSE(list_status.ok());
      EXPECT_EQ(list_status.error_code(), grpc::StatusCode::UNAUTHENTICATED);
      EXPECT_NE(list_status.error_message().find("mfa_required"), std::string::npos)
          << list_status.error_message();

      // SubmitMfa itself is still reachable: a wrong code fails as a TOTP
      // failure, not as "MFA required before this operation".
      grpc::ClientContext mfa_ctx;
      set_bearer(mfa_ctx, token);
      fmgr::v1::SubmitMfaRequest mfa_req;
      mfa_req.set_totp_code("000000");
      fmgr::v1::SubmitMfaResponse mfa_resp;
      const auto mfa_status = auth_stub_->SubmitMfa(&mfa_ctx, mfa_req, &mfa_resp);
      ASSERT_FALSE(mfa_status.ok());
      EXPECT_EQ(mfa_status.error_code(), grpc::StatusCode::UNAUTHENTICATED);
      EXPECT_EQ(mfa_status.error_message().find("mfa_required"), std::string::npos)
          << mfa_status.error_message();
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

    // ---- #140: AuthService.WhoAmI ----

    // The SPA cannot read its own session cookie, so after a reload only the
    // server can say who is signed in. This is the acceptance test for the whole
    // answer: the caller's own identity, every lab they belong to with the role
    // they hold there, and the permission keys the server enforces. The caller is
    // deliberately in **two** labs — the demo's blocker B3 was a session with
    // exactly one fake lab, and an RPC that reported one lab would have passed a
    // weaker test.
    TEST_F(ServerIntegrationTest, WhoAmIListsEveryLabTheCallerBelongsToWithItsRole) {
      seed_two_lab_caller();
      const auto token = login(kTwoLabEmail, kPassword);
      ASSERT_FALSE(token.empty());

      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::WhoAmIRequest req;
      fmgr::v1::WhoAmIResponse resp;
      const auto status = auth_stub_->WhoAmI(&ctx, req, &resp);
      ASSERT_TRUE(status.ok()) << status.error_message();

      EXPECT_EQ(resp.user_id(), kTwoLabUserId);
      EXPECT_EQ(resp.email(), kTwoLabEmail);
      EXPECT_EQ(resp.display_name(), "Two Lab User");
      EXPECT_FALSE(resp.is_system_admin())
          << "administrating a lab is not deployment administration";
      EXPECT_EQ(resp.permissions_size(), 0)
          << "a lab role grants nothing deployment-wide; lab grants belong to their lab";

      ASSERT_EQ(resp.labs_size(), 2) << "a caller in two labs must get both";
      const auto* first = lab_entry(resp, kLabId);
      const auto* second = lab_entry(resp, kSecondLabId);
      ASSERT_NE(first, nullptr) << "the lab the caller administrates is missing";
      ASSERT_NE(second, nullptr) << "the second lab the caller belongs to is missing";

      // LabAdmin in the first lab: its grants include user management.
      EXPECT_EQ(first->lab_name(), "Test Lab");
      EXPECT_EQ(first->role_id(), core::builtin_role_id(core::RoleKind::LabAdmin).to_string());
      EXPECT_EQ(first->role_name(), "LabAdmin");
      EXPECT_FALSE(first->is_phi_enabled());
      EXPECT_TRUE(holds(*first, "sample.read"));
      EXPECT_TRUE(holds(*first, "user.invite"));
      EXPECT_FALSE(holds(*first, "sample.delete_hard"))
          << "a global-only permission must never appear as a lab grant";

      // Member in the second lab: read/write, no user management, and the lab's
      // PHI flag and the membership's scope filter come from that lab's rows.
      EXPECT_EQ(second->lab_name(), "Second Lab");
      EXPECT_EQ(second->role_id(), core::builtin_role_id(core::RoleKind::Member).to_string());
      EXPECT_EQ(second->role_name(), "Member");
      EXPECT_TRUE(second->is_phi_enabled());
      EXPECT_TRUE(holds(*second, "sample.read"));
      EXPECT_FALSE(holds(*second, "user.invite"))
          << "the role in each lab is what decides the permissions in that lab";
      EXPECT_EQ(second->scope_filters_json(), kSecondLabScopeFilter);
    }

    // The same claim as above, planted against a second account: whatever the
    // caller is, the answer describes them and not somebody else. Two sessions
    // call the RPC, and each response is checked to name its own user and to
    // carry none of the other's labs.
    TEST_F(ServerIntegrationTest, WhoAmIDescribesOnlyTheCaller) {
      seed_two_lab_caller();
      const auto admin_token = login(kEmail, kPassword);
      const auto other_token = login(kTwoLabEmail, kPassword);
      ASSERT_FALSE(admin_token.empty());
      ASSERT_FALSE(other_token.empty());

      const auto who_am_i = [this](const std::string& bearer) {
        grpc::ClientContext ctx;
        set_bearer(ctx, bearer);
        fmgr::v1::WhoAmIRequest req;
        fmgr::v1::WhoAmIResponse resp;
        return std::pair{auth_stub_->WhoAmI(&ctx, req, &resp), resp};
      };

      const auto [admin_status, admin_resp] = who_am_i(admin_token);
      ASSERT_TRUE(admin_status.ok()) << admin_status.error_message();
      EXPECT_EQ(admin_resp.user_id(), "10000000-0000-0000-0000-000000000001");
      EXPECT_EQ(admin_resp.email(), kEmail);
      EXPECT_TRUE(admin_resp.is_system_admin());
      EXPECT_EQ(admin_resp.labs_size(), 1)
          << "memberships, not the labs a deployment admin may see: this account "
             "belongs to one lab";
      EXPECT_EQ(lab_entry(admin_resp, kSecondLabId), nullptr)
          << "the other account's lab leaked into this response";
      EXPECT_NE(admin_resp.user_id(), kTwoLabUserId);

      const auto [other_status, other_resp] = who_am_i(other_token);
      ASSERT_TRUE(other_status.ok()) << other_status.error_message();
      EXPECT_EQ(other_resp.user_id(), kTwoLabUserId);
      EXPECT_EQ(other_resp.email(), kTwoLabEmail);
      EXPECT_NE(other_resp.user_id(), admin_resp.user_id());
      // Both accounts are members of kLabId, so it appears in both answers — with
      // each caller's own role in it, never the other's.
      const auto* other_first = lab_entry(other_resp, kLabId);
      ASSERT_NE(other_first, nullptr);
      EXPECT_EQ(other_first->role_name(), "LabAdmin");
      EXPECT_NE(lab_entry(other_resp, kSecondLabId), nullptr);
      const auto* admin_first = lab_entry(admin_resp, kLabId);
      ASSERT_NE(admin_first, nullptr);
      EXPECT_EQ(admin_first->role_name(), "SystemAdmin");
    }

    // The identity is the credential's, so there is nothing to ask *about*: the
    // request message has no fields. Asserted on the descriptor rather than on a
    // comment, so a later PR that adds `user_id` to the request has to delete
    // this test to get green.
    TEST_F(ServerIntegrationTest, WhoAmIRequestHasNoSubjectField) {
      const auto* descriptor = fmgr::v1::WhoAmIRequest::descriptor();
      EXPECT_EQ(descriptor->field_count(), 0)
          << "WhoAmI describes the caller named by the bearer token; a request field "
             "could be used to describe someone else";
    }

    // #140's "no secret material, and no PHI" criterion, pinned by name. The
    // response is a fixed whitelist: it carries no session token, no session id,
    // no token prefix and no password or TOTP state, so a later field of that
    // kind has to fail this test and be argued for in review instead of being
    // added quietly (AGENTS.md §5).
    TEST_F(ServerIntegrationTest, WhoAmIResponseCarriesNoCredentialOrSessionMaterial) {
      const auto field_names = [](const google::protobuf::Descriptor& message) {
        std::vector<std::string> names;
        names.reserve(static_cast<std::size_t>(message.field_count()));
        for (int index = 0; index < message.field_count(); ++index) {
          names.emplace_back(message.field(index)->name());
        }
        return names;
      };

      EXPECT_EQ(field_names(*fmgr::v1::WhoAmIResponse::descriptor()),
                (std::vector<std::string>{"user_id", "email", "display_name", "is_system_admin",
                                          "permissions", "labs"}));
      EXPECT_EQ(field_names(*fmgr::v1::WhoAmIMembership::descriptor()),
                (std::vector<std::string>{"lab_id", "lab_name", "role_id", "role_name",
                                          "permissions", "scope_filters_json", "is_phi_enabled"}));

      // Behavioural half: the session token this caller is holding does not
      // appear anywhere in the serialized answer.
      const auto token = login(kEmail, kPassword);
      ASSERT_FALSE(token.empty());
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::WhoAmIRequest req;
      fmgr::v1::WhoAmIResponse resp;
      ASSERT_TRUE(auth_stub_->WhoAmI(&ctx, req, &resp).ok());
      EXPECT_EQ(resp.SerializeAsString().find(token), std::string::npos)
          << "the bearer token must never be echoed back";
    }

    // Unauthenticated callers get nothing: no identity, no lab list. A token
    // that was never issued must not be distinguishable from no token at all.
    TEST_F(ServerIntegrationTest, WhoAmIRejectsAbsentAndUnissuedCredentials) {
      {
        grpc::ClientContext ctx; // no Authorization header
        fmgr::v1::WhoAmIRequest req;
        fmgr::v1::WhoAmIResponse resp;
        const auto status = auth_stub_->WhoAmI(&ctx, req, &resp);
        EXPECT_EQ(status.error_code(), grpc::StatusCode::UNAUTHENTICATED) << status.error_message();
        EXPECT_EQ(resp.user_id(), "");
        EXPECT_EQ(resp.labs_size(), 0);
      }
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, "fmgr_sess_never-issued");
        fmgr::v1::WhoAmIRequest req;
        fmgr::v1::WhoAmIResponse resp;
        const auto status = auth_stub_->WhoAmI(&ctx, req, &resp);
        EXPECT_EQ(status.error_code(), grpc::StatusCode::UNAUTHENTICATED) << status.error_message();
        EXPECT_EQ(resp.user_id(), "");
        EXPECT_EQ(resp.labs_size(), 0);
      }
    }

    // #60: the count floor is gone. The registry must hold exactly the RPCs the
    // server serves — no fewer (an RPC would be served with no registration for
    // the gate to check against) and no more (an entry for an RPC nobody serves
    // is a claim nothing can honour). The served set comes from the server's own
    // served-service list — the same rows build() registers (#80) — read through
    // the generated proto descriptors, not from a second hand-written copy in the
    // test.
    TEST_F(ServerIntegrationTest, RpcRegistryHoldsExactlyTheServedRpcs) {
      const auto served = server_->served_rpc_names();
      const auto registry = rpc::AuthMiddleware::registered_rpcs();

      EXPECT_FALSE(served.empty()) << "no served RPCs enumerated; the descriptor lookup is broken";

      std::string not_registered;
      for (const auto& name : served) {
        if (!registry.contains(name)) {
          not_registered += name + " ";
        }
      }
      std::string not_served;
      for (const auto& [name, permission] : registry) {
        (void)permission;
        if (std::find(served.begin(), served.end(), name) == served.end()) {
          not_served += name + " ";
        }
      }

      EXPECT_TRUE(not_registered.empty())
          << "served RPC(s) missing from the permission registry: " << not_registered;
      EXPECT_TRUE(not_served.empty())
          << "registered RPC(s) the server does not serve: " << not_served;
      EXPECT_EQ(registry.size(), served.size());
    }

    // #60 acceptance test. The registry is not just documentation any more: the
    // gate (AuthMiddleware::authorize) checks the permission a handler enforces
    // against the permission its RPC registered, and refuses the call when the
    // two disagree. This test plants the disagreement in-process — the same
    // `register_rpc` call the service constructor makes, with the wrong
    // permission — and asserts the call is refused. Before #60 this passed and
    // the suite stayed green, which is exactly how #54 nearly relaxed a mutating
    // RPC.
    TEST_F(ServerIntegrationTest, RegisteredPermissionDisagreeingWithEnforcedPermissionIsRefused) {
      const auto token = login(kEmail, kPassword);
      ASSERT_FALSE(token.empty());

      const std::string sample_read_rpc = "/fmgr.v1.SampleService/ListSamples";
      const std::string sample_write_rpc = "/fmgr.v1.SampleService/CreateSample";

      // Control: ListSamples enforces sample.read and is registered as sample.read,
      // so an authorised call succeeds.
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::ListSamplesRequest req;
        req.set_lab_id(kLabId);
        fmgr::v1::ListSamplesResponse resp;
        const auto status = sample_stub_->ListSamples(&ctx, req, &resp);
        EXPECT_TRUE(status.ok()) << status.error_message();
      }

      // Planted disagreement #1, read path: ListSamples enforces sample.read;
      // register it as freezer.configure.
      rpc::AuthMiddleware::register_rpc(sample_read_rpc, core::Permission::FreezerConfigure);
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::ListSamplesRequest req;
        req.set_lab_id(kLabId);
        fmgr::v1::ListSamplesResponse resp;
        const auto status = sample_stub_->ListSamples(&ctx, req, &resp);
        EXPECT_EQ(status.error_code(), grpc::StatusCode::INTERNAL)
            << "a registered permission that disagrees with the enforced one must be refused; got: "
            << status.error_message();
        EXPECT_NE(status.error_message().find("freezer.configure"), std::string::npos)
            << "the refusal must name the registered permission; got: " << status.error_message();
        EXPECT_NE(status.error_message().find("sample.read"), std::string::npos)
            << "the refusal must name the enforced permission; got: " << status.error_message();
      }
      rpc::AuthMiddleware::register_rpc(sample_read_rpc, core::Permission::SampleRead);

      // Planted disagreement #2, mutating path (#54's near miss): CreateSample
      // enforces sample.write; register it as sample.read. The caller holds both,
      // so only the registry disagreement can refuse the call. The refusal must
      // name both permissions — an empty payload fails with a UUID parse error
      // that names neither, so the message is what distinguishes the two.
      rpc::AuthMiddleware::register_rpc(sample_write_rpc, core::Permission::SampleRead);
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::CreateSampleRequest req;
        req.set_lab_id(kLabId);
        fmgr::v1::CreateSampleResponse resp;
        const auto status = sample_stub_->CreateSample(&ctx, req, &resp);
        EXPECT_EQ(status.error_code(), grpc::StatusCode::INTERNAL)
            << "a mutating RPC whose registration was relaxed must be refused; got: "
            << status.error_message();
        EXPECT_NE(status.error_message().find("sample.read"), std::string::npos)
            << "the refusal must name the registered permission; got: " << status.error_message();
        EXPECT_NE(status.error_message().find("sample.write"), std::string::npos)
            << "the refusal must name the enforced permission; got: " << status.error_message();
      }
      rpc::AuthMiddleware::register_rpc(sample_write_rpc, core::Permission::SampleWrite);

      // Restored: the same authorised call is back to succeeding, so the refusals
      // above came from the planted disagreement and nothing else.
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::ListSamplesRequest req;
        req.set_lab_id(kLabId);
        fmgr::v1::ListSamplesResponse resp;
        const auto status = sample_stub_->ListSamples(&ctx, req, &resp);
        EXPECT_TRUE(status.ok()) << status.error_message();
      }

      // The refusal is INTERNAL in both masking modes; only the detail differs.
      // Masking is process-wide and keys off NDEBUG in production
      // (FreezerServerOptions), so the assertions above depend on the fixture
      // turning it *off* — deliberately, not by inheriting the debug default,
      // which is what made this test fail the release presets in review. With
      // masking on, the client must see the generic message and no permission
      // keys at all.
      server::set_mask_internal_errors(true);
      rpc::AuthMiddleware::register_rpc(sample_read_rpc, core::Permission::FreezerConfigure);
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::ListSamplesRequest req;
        req.set_lab_id(kLabId);
        fmgr::v1::ListSamplesResponse resp;
        const auto status = sample_stub_->ListSamples(&ctx, req, &resp);
        EXPECT_EQ(status.error_code(), grpc::StatusCode::INTERNAL) << status.error_message();
        EXPECT_EQ(status.error_message().find("freezer.configure"), std::string::npos)
            << "masked internal detail must not name the registered permission; got: "
            << status.error_message();
        EXPECT_EQ(status.error_message().find("sample.read"), std::string::npos)
            << "masked internal detail must not name the enforced permission; got: "
            << status.error_message();
      }
      server::set_mask_internal_errors(false);
      rpc::AuthMiddleware::register_rpc(sample_read_rpc, core::Permission::SampleRead);
    }

    // #78/#119: the registrations that used to name a permission no code path
    // enforced — nine then, ten after #140 added WhoAmI. Each states the
    // credential rule its handler applies, and that rule is what the gate applies
    // on every call (#119). This test pins the declaration;
    // CredentialRuleDisagreeingWithEnforcedRuleIsRefused proves the declaration is
    // not the only thing standing between a caller and the RPC, and
    // EveryRpcDeclaringTokenAndMfaRefusesAPendingMfaSession checks the third rule
    // at the boundary it names. The tenth of #78's ten, VerifyAuditChain, is
    // genuinely gated — on the deployment-admin predicate this repo spells
    // has_global(lab.provision) — and its handler asks authorize() for exactly
    // that, so the #60 check verifies it on every call.
    TEST_F(ServerIntegrationTest, RpcRegistryStatesTheCredentialRuleEachNonPermissionRpcHas) {
      const auto registry = rpc::AuthMiddleware::registered_rpcs();

      const std::array<std::pair<std::string, rpc::CredentialRule>, 10> expected{{
          {"/fmgr.v1.AuthService/Login", rpc::CredentialRule::None},
          {"/fmgr.v1.AuthService/SubmitMfa", rpc::CredentialRule::TokenOnly},
          {"/fmgr.v1.AuthService/Logout", rpc::CredentialRule::TokenOnly},
          {"/fmgr.v1.AuthService/CreateApiToken", rpc::CredentialRule::TokenAndMfa},
          {"/fmgr.v1.AuthService/ListApiTokens", rpc::CredentialRule::TokenAndMfa},
          {"/fmgr.v1.AuthService/RevokeApiToken", rpc::CredentialRule::TokenAndMfa},
          {"/fmgr.v1.AuthService/WhoAmI", rpc::CredentialRule::TokenAndMfa},
          {"/fmgr.v1.SessionService/ListSessions", rpc::CredentialRule::TokenAndMfa},
          {"/fmgr.v1.SessionService/RevokeSession", rpc::CredentialRule::TokenAndMfa},
          {"/fmgr.v1.LabService/ListLabs", rpc::CredentialRule::TokenAndMfa},
      }};
      for (const auto& [rpc, rule] : expected) {
        const auto entry = registry.find(rpc);
        ASSERT_NE(entry, registry.end()) << rpc << " is missing from the RPC registry";
        ASSERT_EQ(entry->second.kind(), rpc::RpcGate::Kind::Credential)
            << rpc << " is registered as " << entry->second.describe()
            << ", i.e. it still claims a permission its handler never enforces";
        EXPECT_EQ(entry->second.credential_rule(), rule)
            << rpc << " is registered as " << entry->second.describe();
      }

      const auto chain = registry.find("/fmgr.v1.AuditService/VerifyAuditChain");
      ASSERT_NE(chain, registry.end());
      ASSERT_EQ(chain->second.kind(), rpc::RpcGate::Kind::Permission);
      EXPECT_EQ(chain->second.permission(), core::Permission::LabProvision)
          << "chain verification is deployment-wide, i.e. system-admin only";
    }

    // #119 acceptance test, the mirror of #60's
    // RegisteredPermissionDisagreeingWithEnforcedPermissionIsRefused for the nine
    // RPCs that never call authorize(). Their registry entry is no longer a
    // declaration: the gate compares it with the credential rule the handler
    // applies on every call, and refuses the call with INTERNAL when they
    // disagree. This test plants the disagreement in-process — the same
    // register_rpc call the service constructor makes, with a different rule — and
    // asserts the call is refused. Before #119 it passed and the suite stayed
    // green, which is exactly how a handler could start demanding a credential its
    // registration did not claim.
    TEST_F(ServerIntegrationTest, CredentialRuleDisagreeingWithEnforcedRuleIsRefused) {
      const std::string submit_mfa_rpc = "/fmgr.v1.AuthService/SubmitMfa";
      const std::string create_api_token_rpc = "/fmgr.v1.AuthService/CreateApiToken";
      const std::string list_samples_rpc = "/fmgr.v1.SampleService/ListSamples";

      // SubmitMfa declares token_only, so a session whose TOTP is outstanding
      // reaches its body. A wrong code then fails as a TOTP check and never as
      // "MFA required", which is what makes it a usable control here.
      const auto pending = login(kMfaEmail, kPassword);
      ASSERT_FALSE(pending.empty());
      const auto submit_mfa = [this](const std::string& bearer) {
        grpc::ClientContext ctx;
        set_bearer(ctx, bearer);
        fmgr::v1::SubmitMfaRequest req;
        req.set_totp_code("000000");
        fmgr::v1::SubmitMfaResponse resp;
        return auth_stub_->SubmitMfa(&ctx, req, &resp);
      };
      {
        const auto status = submit_mfa(pending);
        ASSERT_FALSE(status.ok()) << "a wrong TOTP code must not verify";
        ASSERT_EQ(status.error_code(), grpc::StatusCode::UNAUTHENTICATED) << status.error_message();
        ASSERT_EQ(status.error_message().find("mfa_required"), std::string::npos)
            << "the control must reach the TOTP check, not the MFA gate: "
            << status.error_message();
      }

      // Planted disagreement: the handler applies token_only; register the RPC as
      // token_and_mfa, the rule the pre-#62 handler would have had.
      rpc::AuthMiddleware::register_rpc(submit_mfa_rpc, rpc::RpcGate::token_and_mfa());
      {
        const auto status = submit_mfa(pending);
        EXPECT_EQ(status.error_code(), grpc::StatusCode::INTERNAL)
            << "a handler whose credential rule disagrees with its registration must be refused; "
               "got: "
            << status.error_message();
        EXPECT_NE(status.error_message().find("credential rule 'token_and_mfa'"), std::string::npos)
            << "the refusal must name the registered rule; got: " << status.error_message();
        EXPECT_NE(status.error_message().find("'token_only'"), std::string::npos)
            << "the refusal must name the rule the handler applies; got: "
            << status.error_message();
      }
      rpc::AuthMiddleware::register_rpc(submit_mfa_rpc, rpc::RpcGate::token_only());
      {
        const auto status = submit_mfa(pending);
        EXPECT_EQ(status.error_code(), grpc::StatusCode::UNAUTHENTICATED)
            << "restoring the registration must restore the behaviour, so the refusal above came "
               "from the planted disagreement and nothing else: "
            << status.error_message();
        EXPECT_EQ(status.error_message().find("mfa_required"), std::string::npos)
            << status.error_message();
      }

      const auto token = login(kEmail, kPassword);
      ASSERT_FALSE(token.empty());
      const auto create_api_token = [this, &token]() {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::CreateApiTokenRequest req;
        req.set_name("planted-disagreement");
        req.set_scope_json(R"(["*"])");
        fmgr::v1::CreateApiTokenResponse resp;
        return auth_stub_->CreateApiToken(&ctx, req, &resp);
      };
      ASSERT_TRUE(create_api_token().ok()) << "control: the registration and the handler agree";

      // The other direction of the same disagreement: a registration claiming a
      // weaker rule than the handler applies. Refused, so the state that says
      // "Login is the only anonymous RPC" cannot be used to make one of the
      // token-gated ones anonymous.
      rpc::AuthMiddleware::register_rpc(create_api_token_rpc, rpc::RpcGate::no_credential());
      {
        const auto status = create_api_token();
        EXPECT_EQ(status.error_code(), grpc::StatusCode::INTERNAL) << status.error_message();
        EXPECT_NE(status.error_message().find("credential rule 'no_credential'"), std::string::npos)
            << status.error_message();
        EXPECT_NE(status.error_message().find("'token_and_mfa'"), std::string::npos)
            << status.error_message();
      }

      // And a permission entry for a handler that never calls authorize(): the
      // entry would be enforced by nothing at all, which is #78's failure mode.
      rpc::AuthMiddleware::register_rpc(create_api_token_rpc, core::Permission::SampleRead);
      {
        const auto status = create_api_token();
        EXPECT_EQ(status.error_code(), grpc::StatusCode::INTERNAL) << status.error_message();
        EXPECT_NE(status.error_message().find("permission 'sample.read'"), std::string::npos)
            << status.error_message();
        EXPECT_NE(status.error_message().find("'token_and_mfa'"), std::string::npos)
            << status.error_message();
      }
      rpc::AuthMiddleware::register_rpc(create_api_token_rpc, rpc::RpcGate::token_and_mfa());
      EXPECT_TRUE(create_api_token().ok())
          << "restored, the same call succeeds, so only the registry disagreement refused it";

      // #78's anti-bypass, end to end: a permission-gated handler that calls
      // authorize() is refused when its RPC is registered with a credential rule,
      // so the new state cannot be used to take an RPC out of the #60 check.
      rpc::AuthMiddleware::register_rpc(list_samples_rpc, rpc::RpcGate::token_only());
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::ListSamplesRequest req;
        req.set_lab_id(kLabId);
        fmgr::v1::ListSamplesResponse resp;
        const auto status = sample_stub_->ListSamples(&ctx, req, &resp);
        EXPECT_EQ(status.error_code(), grpc::StatusCode::INTERNAL) << status.error_message();
        EXPECT_NE(status.error_message().find("credential rule 'token_only'"), std::string::npos)
            << status.error_message();
        EXPECT_NE(status.error_message().find("'sample.read'"), std::string::npos)
            << status.error_message();
      }
      rpc::AuthMiddleware::register_rpc(list_samples_rpc, core::Permission::SampleRead);
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::ListSamplesRequest req;
        req.set_lab_id(kLabId);
        fmgr::v1::ListSamplesResponse resp;
        EXPECT_TRUE(sample_stub_->ListSamples(&ctx, req, &resp).ok())
            << "restoring the permission restores the call";
      }
    }

    // The third rule, on every RPC that declares it: a session whose second factor
    // is outstanding is refused at the gate, before the handler runs. The ids in
    // the mutating requests are deliberately not real ones — the credential gate is
    // step 0, so a NOT_FOUND or a parse error here would mean the gate never ran.
    // Logout and SubmitMfa are the two exceptions, and they are tested above.
    TEST_F(ServerIntegrationTest, EveryRpcDeclaringTokenAndMfaRefusesAPendingMfaSession) {
      const auto token = login(kMfaEmail, kPassword);
      ASSERT_FALSE(token.empty());
      const std::string absent_id = "00000000-0000-0000-0000-0000000000ff";

      const auto expect_mfa_refusal = [](const grpc::Status& status, std::string_view rpc) {
        EXPECT_EQ(status.error_code(), grpc::StatusCode::UNAUTHENTICATED)
            << rpc << ": " << status.error_message();
        EXPECT_NE(status.error_message().find("mfa_required"), std::string::npos)
            << rpc << ": " << status.error_message();
      };

      {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::CreateApiTokenRequest req;
        req.set_name("must-not-exist");
        req.set_scope_json(R"(["*"])");
        fmgr::v1::CreateApiTokenResponse resp;
        expect_mfa_refusal(auth_stub_->CreateApiToken(&ctx, req, &resp), "CreateApiToken");
      }
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::ListApiTokensRequest req;
        fmgr::v1::ListApiTokensResponse resp;
        expect_mfa_refusal(auth_stub_->ListApiTokens(&ctx, req, &resp), "ListApiTokens");
      }
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::RevokeApiTokenRequest req;
        req.set_api_token_id(absent_id);
        fmgr::v1::RevokeApiTokenResponse resp;
        expect_mfa_refusal(auth_stub_->RevokeApiToken(&ctx, req, &resp), "RevokeApiToken");
      }
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::ListSessionsRequest req;
        fmgr::v1::ListSessionsResponse resp;
        expect_mfa_refusal(session_stub_->ListSessions(&ctx, req, &resp), "ListSessions");
      }
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::RevokeSessionRequest req;
        req.set_session_id(absent_id);
        fmgr::v1::RevokeSessionResponse resp;
        expect_mfa_refusal(session_stub_->RevokeSession(&ctx, req, &resp), "RevokeSession");
      }
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::ListLabsRequest req;
        fmgr::v1::ListLabsResponse resp;
        expect_mfa_refusal(lab_stub_->ListLabs(&ctx, req, &resp), "ListLabs");
      }
      {
        // #140: a half-finished login must not be able to enumerate an identity,
        // so WhoAmI is on this list rather than returning an empty answer for a
        // pending-MFA session.
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::WhoAmIRequest req;
        fmgr::v1::WhoAmIResponse resp;
        expect_mfa_refusal(auth_stub_->WhoAmI(&ctx, req, &resp), "WhoAmI");
        EXPECT_EQ(resp.user_id(), "") << "the refusal must not carry an identity";
        EXPECT_EQ(resp.labs_size(), 0);
      }
    }

    // #78 criterion 3, still standing after #119 turned the entries into enforced
    // rules: every one of them has a test pinning its actual gate.
    // The caller below holds no permission whatsoever — it has no lab membership,
    // so resolve_permissions() grants it nothing — and the control at the end
    // proves that is real by showing a permission-gated RPC refuses it.
    TEST_F(ServerIntegrationTest, PermissionlessCallerReachesEveryRpcThatRequiresNoPermission) {
      const auto token = login(kNoPermissionEmail, kPassword);
      ASSERT_FALSE(token.empty()) << "Login needs no permission; it is where a caller gets one";

      // WhoAmI (#140): self-management. A caller with no membership still learns
      // who they are; they simply belong to no lab and hold nothing.
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::WhoAmIRequest req;
        fmgr::v1::WhoAmIResponse resp;
        const auto status = auth_stub_->WhoAmI(&ctx, req, &resp);
        EXPECT_TRUE(status.ok()) << status.error_message();
        EXPECT_EQ(resp.user_id(), kNoPermissionUserId);
        EXPECT_EQ(resp.email(), kNoPermissionEmail);
        EXPECT_FALSE(resp.is_system_admin());
        EXPECT_EQ(resp.permissions_size(), 0);
        EXPECT_EQ(resp.labs_size(), 0) << "no membership means no lab entry, not a refusal";
      }

      // ListSessions: the caller's own rows, and only those.
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::ListSessionsRequest req;
        fmgr::v1::ListSessionsResponse resp;
        const auto status = session_stub_->ListSessions(&ctx, req, &resp);
        EXPECT_TRUE(status.ok()) << status.error_message();
        ASSERT_GE(resp.sessions_size(), 1);
        EXPECT_EQ(resp.sessions(0).user_id(), kNoPermissionUserId);
      }

      // ListLabs: visibility-scoped, not gated — an empty membership list means
      // an empty result, not a refusal.
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::ListLabsRequest req;
        fmgr::v1::ListLabsResponse resp;
        const auto status = lab_stub_->ListLabs(&ctx, req, &resp);
        EXPECT_TRUE(status.ok()) << status.error_message();
        EXPECT_EQ(resp.labs_size(), 0) << "a caller with no membership sees no labs";
      }

      // Self-management: the caller's own API tokens, created and revoked.
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::ListApiTokensRequest req;
        fmgr::v1::ListApiTokensResponse resp;
        const auto status = auth_stub_->ListApiTokens(&ctx, req, &resp);
        EXPECT_TRUE(status.ok()) << status.error_message();
        EXPECT_EQ(resp.tokens_size(), 0);
      }
      std::string api_token_id;
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::CreateApiTokenRequest req;
        req.set_name("no-role");
        req.set_scope_json(R"(["*"])");
        fmgr::v1::CreateApiTokenResponse resp;
        const auto status = auth_stub_->CreateApiToken(&ctx, req, &resp);
        ASSERT_TRUE(status.ok()) << status.error_message();
        api_token_id = resp.api_token_id();
      }
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::RevokeApiTokenRequest req;
        req.set_api_token_id(api_token_id);
        fmgr::v1::RevokeApiTokenResponse resp;
        const auto status = auth_stub_->RevokeApiToken(&ctx, req, &resp);
        EXPECT_TRUE(status.ok()) << status.error_message();
      }

      // SubmitMfa: reachable with the session token, because completing MFA is
      // what it does. This account has no TOTP secret, so it fails on the code —
      // an authentication error, not a permission one.
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::SubmitMfaRequest req;
        req.set_totp_code("000000");
        fmgr::v1::SubmitMfaResponse resp;
        const auto status = auth_stub_->SubmitMfa(&ctx, req, &resp);
        EXPECT_EQ(status.error_code(), grpc::StatusCode::UNAUTHENTICATED) << status.error_message();
      }

      // Control: the same caller, same credentials, is refused where a permission
      // is required — so the successes above are "no permission needed", not
      // "permissions are ignored".
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::ListSamplesRequest req;
        req.set_lab_id(kLabId);
        fmgr::v1::ListSamplesResponse resp;
        const auto status = sample_stub_->ListSamples(&ctx, req, &resp);
        EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED)
            << status.error_message();
      }

      // Logout ends this session; re-login for the last two, which are per-session.
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::LogoutRequest req;
        fmgr::v1::LogoutResponse resp;
        const auto status = auth_stub_->Logout(&ctx, req, &resp);
        EXPECT_TRUE(status.ok()) << status.error_message();
      }
      const auto second = login(kNoPermissionEmail, kPassword);
      ASSERT_FALSE(second.empty());
      {
        grpc::ClientContext list_ctx;
        set_bearer(list_ctx, second);
        fmgr::v1::ListSessionsRequest list_req;
        fmgr::v1::ListSessionsResponse list_resp;
        ASSERT_TRUE(session_stub_->ListSessions(&list_ctx, list_req, &list_resp).ok());
        ASSERT_GE(list_resp.sessions_size(), 1);

        // RevokeSession: its own session. session.revoke (#77) is what another
        // user's session costs, and this caller does not hold it.
        grpc::ClientContext ctx;
        set_bearer(ctx, second);
        fmgr::v1::RevokeSessionRequest req;
        req.set_session_id(list_resp.sessions(0).id());
        fmgr::v1::RevokeSessionResponse resp;
        const auto status = session_stub_->RevokeSession(&ctx, req, &resp);
        EXPECT_TRUE(status.ok()) << status.error_message();
      }
    }

    // Security audit H-1: a burst of Login attempts from one source is throttled
    // with RESOURCE_EXHAUSTED once the per-IP token bucket drains, regardless of
    // which account each attempt targets.
    TEST_F(ServerIntegrationTest, LoginBurstFromOneSourceIsRateLimited) {
      const int attempts = static_cast<int>(server::AuthServiceImpl::k_login_rate_capacity) + 20;
      int unauthenticated = 0;
      int resource_exhausted = 0;
      for (int i = 0; i < attempts; ++i) {
        grpc::ClientContext ctx;
        fmgr::v1::LoginRequest req;
        req.set_email("sprayed-" + std::to_string(i) + "@example.com");
        req.set_password("wrong-password");
        fmgr::v1::LoginResponse resp;
        const auto status = auth_stub_->Login(&ctx, req, &resp);
        if (status.error_code() == grpc::StatusCode::RESOURCE_EXHAUSTED) {
          ++resource_exhausted;
        } else if (status.error_code() == grpc::StatusCode::UNAUTHENTICATED) {
          ++unauthenticated;
        }
      }
      // The first ~capacity attempts reach the auth layer (wrong creds ->
      // UNAUTHENTICATED); the overflow is throttled before any work is done.
      EXPECT_GT(unauthenticated, 0);
      EXPECT_GT(resource_exhausted, 0);
    }

    // Security audit H-2: a production deployment that requires TLS must refuse
    // to start a plaintext listener if the cert/key paths are missing.
    TEST(FreezerServerTlsGuard, RequireTlsWithoutCertThrowsBeforeBinding) {
      storage::SqliteBackend backend(storage::SqliteBackendOptions{.database_path = ":memory:"});
      backend.migrate_to_latest();
      auth::LocalAuthProvider provider(backend, fast_config());

      server::FreezerServerOptions opts;
      opts.listen_address = "localhost:0";
      opts.require_tls = true; // cert/key paths intentionally left empty

      server::FreezerServer server(backend, provider, std::move(opts));
      EXPECT_THROW(server.build(), std::invalid_argument);
      // bound_port() stays 0 — the guard fired before AddListeningPort.
      EXPECT_EQ(server.bound_port(), 0);
    }

    // Security audit C-13: the ResourceQuota limits are a byte budget and a
    // thread budget, and neither may be derived from the other. The thread
    // default in particular must not move when the message-size cap changes.
    TEST(FreezerServerResourceLimits, ThreadCountIsIndependentOfMessageSize) {
      const server::FreezerServerOptions defaults;
      EXPECT_EQ(defaults.max_grpc_threads, 64);

      server::FreezerServerOptions bigger_messages;
      bigger_messages.max_receive_message_bytes = std::size_t{64} * 1024 * 1024;
      EXPECT_EQ(bigger_messages.max_grpc_threads, defaults.max_grpc_threads);

      // The memory pool is bounded, and bounded in bytes.
      EXPECT_EQ(defaults.max_grpc_memory_bytes, std::size_t{512} * 1024 * 1024);
      // Both directions are capped, not just receive.
      EXPECT_EQ(defaults.max_send_message_bytes, defaults.max_receive_message_bytes);
    }

    // A request larger than the inbound cap is rejected by gRPC itself, before
    // the payload is buffered or any handler runs.
    TEST(FreezerServerResourceLimits, OversizedRequestIsRejected) {
      storage::SqliteBackend backend(storage::SqliteBackendOptions{.database_path = ":memory:"});
      backend.migrate_to_latest();
      auth::LocalAuthProvider provider(backend, fast_config());

      server::FreezerServerOptions opts;
      opts.listen_address = "localhost:0";
      opts.max_receive_message_bytes = 4096;

      server::FreezerServer server(backend, provider, std::move(opts));
      server.build();
      std::thread server_thread([&server] { server.wait(); });

      const std::string addr = "localhost:" + std::to_string(server.bound_port());
      auto stub = fmgr::v1::AuthService::NewStub(
          grpc::CreateChannel(addr, grpc::InsecureChannelCredentials()));

      grpc::ClientContext ctx;
      fmgr::v1::LoginRequest req;
      req.set_email(std::string(std::size_t{64} * 1024, 'a') + "@example.com");
      req.set_password("irrelevant");
      fmgr::v1::LoginResponse resp;
      const auto status = stub->Login(&ctx, req, &resp);

      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::RESOURCE_EXHAUSTED);

      server.shutdown();
      server_thread.join();
    }

    // Security audit H-3: the error funnel must log internal detail server-side
    // (now via spdlog) but never leak it to the client-facing status message.
    TEST(GrpcErrorTranslation, UnknownExceptionMapsToGenericInternalWithoutLeak) {
      grpc::Status status;
      try {
        throw std::runtime_error("table=users column=ssn value=secret-detail");
      } catch (...) {
        status = server::current_exception_to_grpc_status();
      }
      EXPECT_EQ(status.error_code(), grpc::StatusCode::INTERNAL);
      EXPECT_EQ(status.error_message(), "internal server error");
      EXPECT_EQ(status.error_message().find("ssn"), std::string::npos);
      EXPECT_EQ(status.error_message().find("secret-detail"), std::string::npos);
    }

    TEST(FreezerServerTlsGuard, NoRequireTlsStartsPlaintextInDevMode) {
      storage::SqliteBackend backend(storage::SqliteBackendOptions{.database_path = ":memory:"});
      backend.migrate_to_latest();
      auth::LocalAuthProvider provider(backend, fast_config());

      server::FreezerServerOptions opts;
      opts.listen_address = "localhost:0";
      opts.require_tls = false;

      server::FreezerServer server(backend, provider, std::move(opts));
      EXPECT_NO_THROW(server.build());
      EXPECT_GT(server.bound_port(), 0);
      server.shutdown();
    }

  } // namespace
} // namespace fmgr::test
