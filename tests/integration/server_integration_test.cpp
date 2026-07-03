// SPDX-License-Identifier: AGPL-3.0-or-later

#include "auth/LocalAuthProvider.h"
#include "support/FastAuth.h"
#include "support/RegisterRepositories.h"
#include "support/TempSqliteDb.h"
#include "core/identity.h"
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
#include "storage/sqlite/LoginAttemptRepositories.h"
#include "storage/sqlite/RoleRepositories.h"
#include "storage/sqlite/SampleRepositories.h"
#include "storage/sqlite/SessionRepositories.h"
#include "storage/sqlite/ShareRequestRepositories.h"
#include "storage/sqlite/SqliteBackend.h"

#include "rpc/AuthMiddleware.h"

#include <fmgr/v1/auth.grpc.pb.h>
#include <fmgr/v1/session.grpc.pb.h>
#include <google/protobuf/descriptor.h>
#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

namespace fmgr::test {
  namespace {



    // Fixture that spins up an in-process FreezerServer on a random port.
    class ServerIntegrationTest : public ::testing::Test {
    protected:
      void SetUp() override {
        db_ = std::make_unique<TempSqliteDb>("fmgr-srv-test");

        backend_ = std::make_unique<storage::SqliteBackend>(
            storage::SqliteBackendOptions{.database_path = db_->string()});
        register_all_sqlite_repositories(*backend_);
        backend_->migrate_to_latest();

        provider_ = std::make_unique<auth::LocalAuthProvider>(*backend_, fast_auth_config());

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
        db_.reset();
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

      std::unique_ptr<TempSqliteDb> db_;
      std::unique_ptr<storage::SqliteBackend> backend_;
      std::unique_ptr<auth::LocalAuthProvider> provider_;
      server::FreezerServerOptions server_opts_;
      std::unique_ptr<server::FreezerServer> server_;
      std::thread server_thread_;
      std::shared_ptr<grpc::Channel> channel_;
      std::unique_ptr<fmgr::v1::AuthService::Stub> auth_stub_;
      std::unique_ptr<fmgr::v1::SessionService::Stub> session_stub_;

    private:


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
      // Walk the generated gRPC service descriptors and assert every method has an
      // explicit AuthMiddleware policy. A newly added RPC (or a whole service)
      // fails here unless it registers a policy — nothing reaches a handler
      // ungated by omission. Replaces the previous size>=60 magnitude heuristic.
      const auto registry = rpc::AuthMiddleware::registered_rpcs();

      static constexpr std::array<const char*, 9> kServices{
          "fmgr.v1.AuthService",   "fmgr.v1.SessionService", "fmgr.v1.LabService",
          "fmgr.v1.SampleService", "fmgr.v1.BoxService",     "fmgr.v1.ItemTypeService",
          "fmgr.v1.RoleService",   "fmgr.v1.AuditService",   "fmgr.v1.ShareService"};

      const auto* pool = google::protobuf::DescriptorPool::generated_pool();
      ASSERT_NE(pool, nullptr);

      std::size_t method_total = 0;
      for (const char* service_name : kServices) {
        const auto* service = pool->FindServiceByName(service_name);
        ASSERT_NE(service, nullptr) << "service descriptor not linked: " << service_name;
        for (int i = 0; i < service->method_count(); ++i) {
          const std::string full = "/" + std::string(service_name) + "/" +
                                   std::string(service->method(i)->name());
          ++method_total;
          EXPECT_TRUE(registry.contains(full))
              << "gRPC method has no AuthMiddleware policy: " << full;
        }
      }

      // Exactness: the registry holds exactly the real RPCs (no stray entries).
      EXPECT_EQ(registry.size(), method_total);
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
      auth::LocalAuthProvider provider(backend, fast_auth_config());

      server::FreezerServerOptions opts;
      opts.listen_address = "localhost:0";
      opts.require_tls = true; // cert/key paths intentionally left empty

      server::FreezerServer server(backend, provider, std::move(opts));
      EXPECT_THROW(server.build(), std::invalid_argument);
      // bound_port() stays 0 — the guard fired before AddListeningPort.
      EXPECT_EQ(server.bound_port(), 0);
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
      auth::LocalAuthProvider provider(backend, fast_auth_config());

      server::FreezerServerOptions opts;
      opts.listen_address = "localhost:0";
      opts.require_tls = false;

      server::FreezerServer server(backend, provider, std::move(opts));
      EXPECT_NO_THROW(server.build());
      EXPECT_GT(server.bound_port(), 0);
      server.shutdown();
    }

    // ---- TLS transport (security audit C-9) ----

    namespace {

      [[nodiscard]] std::string tls_fixture(const char* name) {
        return std::string(FMGR_TLS_FIXTURE_DIR) + "/" + name;
      }

      [[nodiscard]] std::string read_pem(const char* name) {
        std::ifstream file(tls_fixture(name), std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
      }

      // A TLS-enabled FreezerServer plus its backing store and one seeded
      // SystemAdmin, all kept alive for the duration of a test. The server runs on
      // an OS-assigned port and its wait() loop on a background thread.
      class TlsServerHarness {
      public:
        static constexpr const char* kEmail = "admin@example.com";
        static constexpr const char* kPassword = "hunter22";

        explicit TlsServerHarness(server::MtlsMode mtls) {
          backend_ = std::make_unique<storage::SqliteBackend>(
              storage::SqliteBackendOptions{.database_path = ":memory:"});
          register_repositories(*backend_);
          backend_->migrate_to_latest();
          provider_ = std::make_unique<auth::LocalAuthProvider>(*backend_, fast_auth_config());
          seed_admin();

          server::FreezerServerOptions opts;
          opts.listen_address = "localhost:0";
          opts.tls_cert_path = tls_fixture("server-cert.pem");
          opts.tls_key_path = tls_fixture("server-key.pem");
          opts.tls_client_ca_path = tls_fixture("ca-cert.pem");
          opts.mtls = mtls;
          server_ = std::make_unique<server::FreezerServer>(*backend_, *provider_, std::move(opts));
          server_->build();
          thread_ = std::thread([this] { server_->wait(); });
        }

        ~TlsServerHarness() {
          server_->shutdown();
          if (thread_.joinable()) {
            thread_.join();
          }
        }

        TlsServerHarness(const TlsServerHarness&) = delete;
        TlsServerHarness& operator=(const TlsServerHarness&) = delete;

        [[nodiscard]] std::string address() const {
          return "localhost:" + std::to_string(server_->bound_port());
        }

      private:
        static void register_repositories(storage::SqliteBackend& b) {
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

        void seed_admin() {
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

        std::unique_ptr<storage::SqliteBackend> backend_;
        std::unique_ptr<auth::LocalAuthProvider> provider_;
        std::unique_ptr<server::FreezerServer> server_;
        std::thread thread_;
      };

      // Client-side TLS credentials trusting the test CA. When present_client_cert
      // is true, also present the test client cert/key for mTLS.
      [[nodiscard]] std::shared_ptr<grpc::ChannelCredentials>
      tls_client_creds(bool present_client_cert) {
        grpc::SslCredentialsOptions opts;
        opts.pem_root_certs = read_pem("ca-cert.pem");
        if (present_client_cert) {
          opts.pem_private_key = read_pem("client-key.pem");
          opts.pem_cert_chain = read_pem("client-cert.pem");
        }
        return grpc::SslCredentials(opts);
      }

      // Attempt a Login and return the resulting gRPC status. A short deadline
      // keeps a failed handshake from hanging the test.
      [[nodiscard]] grpc::Status try_login(const std::shared_ptr<grpc::Channel>& channel) {
        auto stub = fmgr::v1::AuthService::NewStub(channel);
        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
        fmgr::v1::LoginRequest req;
        req.set_email(TlsServerHarness::kEmail);
        req.set_password(TlsServerHarness::kPassword);
        fmgr::v1::LoginResponse resp;
        return stub->Login(&ctx, req, &resp);
      }

    } // namespace

    TEST(FreezerServerTls, AcceptsTlsClientAndRoundTripsRpc) {
      TlsServerHarness harness(server::MtlsMode::Off);
      auto channel = grpc::CreateChannel(harness.address(), tls_client_creds(false));
      const auto status = try_login(channel);
      // Handshake succeeded and the RPC reached the service: valid creds -> OK.
      EXPECT_TRUE(status.ok()) << status.error_code() << ": " << status.error_message();
    }

    TEST(FreezerServerTls, RejectsPlaintextClient) {
      TlsServerHarness harness(server::MtlsMode::Off);
      auto channel = grpc::CreateChannel(harness.address(), grpc::InsecureChannelCredentials());
      const auto status = try_login(channel);
      // A plaintext client cannot complete the TLS handshake: transport failure,
      // not an application-level UNAUTHENTICATED.
      EXPECT_FALSE(status.ok());
      EXPECT_NE(status.error_code(), grpc::StatusCode::UNAUTHENTICATED);
    }

    TEST(FreezerServerTls, MtlsRequireRejectsClientWithoutCert) {
      TlsServerHarness harness(server::MtlsMode::Require);
      auto channel = grpc::CreateChannel(harness.address(), tls_client_creds(false));
      const auto status = try_login(channel);
      EXPECT_FALSE(status.ok());
      EXPECT_NE(status.error_code(), grpc::StatusCode::UNAUTHENTICATED);
    }

    TEST(FreezerServerTls, MtlsRequireAcceptsClientWithCert) {
      TlsServerHarness harness(server::MtlsMode::Require);
      auto channel = grpc::CreateChannel(harness.address(), tls_client_creds(true));
      const auto status = try_login(channel);
      EXPECT_TRUE(status.ok()) << status.error_code() << ": " << status.error_message();
    }

    TEST(FreezerServerTls, MtlsOffIgnoresClientCert) {
      TlsServerHarness harness(server::MtlsMode::Off);
      // A plain server-auth TLS client (no client cert) succeeds.
      auto channel = grpc::CreateChannel(harness.address(), tls_client_creds(false));
      const auto status = try_login(channel);
      EXPECT_TRUE(status.ok()) << status.error_code() << ": " << status.error_message();
    }

    TEST(FreezerServerTls, RequireCaMissingInRequireModeThrows) {
      storage::SqliteBackend backend(storage::SqliteBackendOptions{.database_path = ":memory:"});
      backend.migrate_to_latest();
      auth::LocalAuthProvider provider(backend, fast_auth_config());

      server::FreezerServerOptions opts;
      opts.listen_address = "localhost:0";
      opts.tls_cert_path = tls_fixture("server-cert.pem");
      opts.tls_key_path = tls_fixture("server-key.pem");
      opts.mtls = server::MtlsMode::Require; // tls_client_ca_path intentionally empty

      server::FreezerServer server(backend, provider, std::move(opts));
      EXPECT_THROW(server.build(), std::invalid_argument);
    }

  } // namespace
} // namespace fmgr::test
