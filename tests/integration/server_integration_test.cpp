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
#include <thread>
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

    // #78: the ten registrations that used to name a permission no code path
    // enforced. Nine of them now state that they require no permission at all;
    // the tenth, VerifyAuditChain, is genuinely gated — on the deployment-admin
    // predicate this repo spells has_global(lab.provision) — and its handler asks
    // authorize() for exactly that, so the gate verifies the entry on every call.
    // This is the test that goes red when one of them claims a permission its
    // handler does not enforce again.
    TEST_F(ServerIntegrationTest, RpcRegistryStatesTheGateEachNonPermissionRpcHas) {
      const auto registry = rpc::AuthMiddleware::registered_rpcs();

      const std::array<std::string, 9> requires_no_permission{
          "/fmgr.v1.AuthService/Login",           "/fmgr.v1.AuthService/SubmitMfa",
          "/fmgr.v1.AuthService/Logout",          "/fmgr.v1.AuthService/CreateApiToken",
          "/fmgr.v1.AuthService/ListApiTokens",   "/fmgr.v1.AuthService/RevokeApiToken",
          "/fmgr.v1.SessionService/ListSessions", "/fmgr.v1.SessionService/RevokeSession",
          "/fmgr.v1.LabService/ListLabs",
      };
      for (const auto& rpc : requires_no_permission) {
        const auto entry = registry.find(rpc);
        ASSERT_NE(entry, registry.end()) << rpc << " is missing from the RPC registry";
        EXPECT_TRUE(entry->second.kind() == rpc::RpcGate::Kind::NoPermissionRequired)
            << rpc << " is registered as " << entry->second.describe()
            << ", i.e. it still claims a permission its handler never enforces";
      }

      const auto chain = registry.find("/fmgr.v1.AuditService/VerifyAuditChain");
      ASSERT_NE(chain, registry.end());
      ASSERT_EQ(chain->second.kind(), rpc::RpcGate::Kind::Permission);
      EXPECT_EQ(chain->second.permission(), core::Permission::LabProvision)
          << "chain verification is deployment-wide, i.e. system-admin only";
    }

    // #78 criterion 3: every one of the ten has a test pinning its actual gate.
    // The caller below holds no permission whatsoever — it has no lab membership,
    // so resolve_permissions() grants it nothing — and the control at the end
    // proves that is real by showing a permission-gated RPC refuses it.
    TEST_F(ServerIntegrationTest, PermissionlessCallerReachesEveryRpcThatRequiresNoPermission) {
      const auto token = login(kNoPermissionEmail, kPassword);
      ASSERT_FALSE(token.empty()) << "Login needs no permission; it is where a caller gets one";

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
