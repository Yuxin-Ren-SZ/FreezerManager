// SPDX-License-Identifier: AGPL-3.0-or-later

// End-to-end tests for the REST/JSON gateway. A single Drogon app (process
// singleton) is stood up once via a global test environment: it fronts a real
// in-process FreezerServer over the gRPC in-process channel, so these tests
// exercise the full path JSON -> proto -> RBAC gate -> repo/audit/txn -> JSON.
//
// Per PRD §15 every exposed endpoint gets a positive and a negative
// authorization test (a caller who may, one who may not, plus missing bearer).

#include "auth/LocalAuthProvider.h"
#include "core/identity.h"
#include "core/role.h"
#include "rest/GatewayStubs.h"
#include "rest/RestGateway.h"
#include "server/FreezerServer.h"
#include "storage/IdentityTraits.h"
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

#include <drogon/HttpClient.h>
#include <drogon/HttpRequest.h>
#include <drogon/drogon.h>
#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <utility>

namespace fmgr::test {
  namespace {

    [[nodiscard]] auth::LocalAuthProviderConfig fast_config() {
      auth::LocalAuthProviderConfig cfg;
      cfg.pwhash_memlimit = 8192;
      cfg.pwhash_opslimit = 1;
      return cfg;
    }

    // Ask the OS for a free loopback TCP port, then hand it to Drogon. A tiny
    // race window exists between close() and Drogon's bind(), acceptable here.
    [[nodiscard]] std::uint16_t find_free_port() {
      const int sock_fd = ::socket(AF_INET, SOCK_STREAM, 0);
      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      addr.sin_port = 0;
      (void)::bind(sock_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
      socklen_t len = sizeof(addr);
      (void)::getsockname(sock_fd, reinterpret_cast<sockaddr*>(&addr), &len);
      const std::uint16_t port = ntohs(addr.sin_port);
      ::close(sock_fd);
      return port;
    }

    // Stands up the whole stack once for the test program.
    class RestGatewayEnv : public ::testing::Environment {
    public:
      static RestGatewayEnv* instance;

      const std::string kAdminEmail{"admin@example.com"};
      const std::string kMemberEmail{"member@example.com"};
      const std::string kPassword{"hunter22"};
      const std::string kLabId{"20000000-0000-0000-0000-000000000001"};

      [[nodiscard]] std::string base_url() const {
        return "http://127.0.0.1:" + std::to_string(port_);
      }

      void SetUp() override {
        instance = this;
        db_path_ = std::filesystem::temp_directory_path() / "fmgr-rest-it.db";
        remove_db();

        backend_ = std::make_unique<storage::SqliteBackend>(
            storage::SqliteBackendOptions{.database_path = db_path_.string()});
        register_all_repositories(*backend_);
        backend_->migrate_to_latest();
        provider_ = std::make_unique<auth::LocalAuthProvider>(*backend_, fast_config());
        seed();

        server_opts_.listen_address = "localhost:0";
        server_ = std::make_unique<server::FreezerServer>(*backend_, *provider_, server_opts_);
        server_->build();

        stubs_ = std::make_unique<rest::GatewayStubs>(server_->in_process_channel());
        gateway_ = std::make_unique<rest::RestGateway>(*stubs_);
        gateway_->register_routes();

        port_ = find_free_port();
        drogon::app().addListener("127.0.0.1", port_);
        drogon::app().setThreadNum(1);
        app_thread_ = std::thread([] { drogon::app().run(); });

        // Wait until the event loop is actually running before issuing requests.
        for (int i = 0; i < 200 && !drogon::app().getLoop()->isRunning(); ++i) {
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
      }

      void TearDown() override {
        drogon::app().quit();
        if (app_thread_.joinable()) {
          app_thread_.join();
        }
        if (server_) {
          server_->shutdown();
        }
        server_.reset();
        provider_.reset();
        backend_.reset();
        remove_db();
      }

    private:
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

      void remove_db() {
        std::error_code errc;
        std::filesystem::remove(db_path_, errc);
        std::filesystem::remove(std::filesystem::path(db_path_.string() + "-wal"), errc);
        std::filesystem::remove(std::filesystem::path(db_path_.string() + "-shm"), errc);
      }

      void seed() {
        const auto hash = provider_->hash_password(kPassword);
        const core::LabId lab_id = core::LabId::parse(kLabId);
        const core::UserId admin_id = core::UserId::parse("10000000-0000-0000-0000-000000000001");
        const core::UserId member_id = core::UserId::parse("10000000-0000-0000-0000-000000000002");

        const auto make_user = [&hash](const core::UserId& id, const std::string& email) {
          return core::User{
              .id = id,
              .primary_email = email,
              .display_name = email,
              .status = core::UserStatus::Active,
              .created_at = core::Timestamp::from_unix_micros(1),
              .auth_bindings = nlohmann::json::array({
                  nlohmann::json::object({{"provider", "local"}, {"hash", hash}}),
              }),
          };
        };
        const auto make_membership = [&lab_id](const core::UserId& uid, core::RoleKind kind) {
          return core::LabMembership{
              .user_id = uid,
              .lab_id = lab_id,
              .role_id = core::builtin_role_id(kind),
              .joined_at = core::Timestamp::from_unix_micros(1),
          };
        };
        const core::Lab lab{
            .id = lab_id,
            .name = "Test Lab",
            .contact = "test@example.com",
            .created_at = core::Timestamp::from_unix_micros(1),
            .settings_json = nlohmann::json::object(),
        };
        const storage::MutationContext ctx{
            .actor_user_id = core::UserId::parse("00000000-0000-0000-0000-000000000000"),
            .actor_session_id = "seed",
            .request_id = "seed",
            .reason = "test setup",
        };
        auto txn = backend_->begin(storage::IsolationLevel::Serializable);
        txn->repo<core::Lab>().insert(lab, ctx);
        txn->repo<core::User>().insert(make_user(admin_id, kAdminEmail), ctx);
        txn->repo<core::User>().insert(make_user(member_id, kMemberEmail), ctx);
        txn->repo<core::LabMembership>().insert(
            make_membership(admin_id, core::RoleKind::SystemAdmin), ctx);
        txn->repo<core::LabMembership>().insert(make_membership(member_id, core::RoleKind::Member),
                                                ctx);
        txn->commit();
      }

      std::filesystem::path db_path_;
      std::unique_ptr<storage::SqliteBackend> backend_;
      std::unique_ptr<auth::LocalAuthProvider> provider_;
      server::FreezerServerOptions server_opts_;
      std::unique_ptr<server::FreezerServer> server_;
      std::unique_ptr<rest::GatewayStubs> stubs_;
      std::unique_ptr<rest::RestGateway> gateway_;
      std::thread app_thread_;
      std::uint16_t port_{0};
    };

    RestGatewayEnv* RestGatewayEnv::instance = nullptr;

    // Registered at static-init time, before gtest_main runs RUN_ALL_TESTS.
    const ::testing::Environment* const kEnv =
        ::testing::AddGlobalTestEnvironment(new RestGatewayEnv);

    // ---- HTTP helper ----

    struct HttpResult {
      int status;
      nlohmann::json body; // null if the body was not valid JSON
      std::string raw;
    };

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    [[nodiscard]] HttpResult post(const std::string& path, const std::string& json_body,
                                  const std::string& bearer = {}) {
      auto* env = RestGatewayEnv::instance;
      auto client = drogon::HttpClient::newHttpClient(env->base_url());
      auto req = drogon::HttpRequest::newHttpRequest();
      req->setMethod(drogon::Post);
      req->setPath(path);
      req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
      req->setBody(json_body);
      if (!bearer.empty()) {
        req->addHeader("Authorization", "Bearer " + bearer);
      }
      auto [result, resp] = client->sendRequest(req, 10.0);
      EXPECT_EQ(result, drogon::ReqResult::Ok);
      HttpResult out;
      out.status = resp ? resp->getStatusCode() : 0;
      out.raw = resp ? std::string(resp->getBody()) : std::string{};
      out.body = nlohmann::json::parse(out.raw, nullptr, /*allow_exceptions=*/false);
      return out;
    }

    [[nodiscard]] std::string login(const std::string& email, const std::string& password) {
      const nlohmann::json req{{"email", email}, {"password", password}};
      const auto res = post("/api/v1/auth/login", req.dump());
      if (res.status != 200 || !res.body.is_object()) {
        return {};
      }
      return res.body.value("session_token", std::string{});
    }

    // ---- Tests ----

    TEST(RestGatewayTest, LoginValidCredentialsReturnsToken) {
      auto* env = RestGatewayEnv::instance;
      const nlohmann::json req{{"email", env->kAdminEmail}, {"password", env->kPassword}};
      const auto res = post("/api/v1/auth/login", req.dump());
      EXPECT_EQ(res.status, 200) << res.raw;
      ASSERT_TRUE(res.body.is_object());
      EXPECT_FALSE(res.body.value("session_token", std::string{}).empty());
    }

    TEST(RestGatewayTest, LoginWrongPasswordReturns401) {
      auto* env = RestGatewayEnv::instance;
      const nlohmann::json req{{"email", env->kAdminEmail}, {"password", "wrong"}};
      const auto res = post("/api/v1/auth/login", req.dump());
      EXPECT_EQ(res.status, 401) << res.raw;
      EXPECT_EQ(res.body.value("code", std::string{}), "UNAUTHENTICATED");
    }

    TEST(RestGatewayTest, MalformedJsonReturns400) {
      const auto res = post("/api/v1/auth/login", "{not json");
      EXPECT_EQ(res.status, 400) << res.raw;
      EXPECT_EQ(res.body.value("code", std::string{}), "INVALID_ARGUMENT");
    }

    TEST(RestGatewayTest, UnknownFieldReturns400) {
      const auto res = post("/api/v1/auth/login", R"({"emial":"x"})");
      EXPECT_EQ(res.status, 400) << res.raw;
    }

    TEST(RestGatewayTest, CreateLabWithoutBearerReturns401) {
      const nlohmann::json req{{"name", "X"}, {"contact", "x@x"}};
      const auto res = post("/api/v1/lab/create", req.dump());
      EXPECT_EQ(res.status, 401) << res.raw;
    }

    TEST(RestGatewayTest, CreateLabAsSystemAdminSucceeds) {
      auto* env = RestGatewayEnv::instance;
      const auto token = login(env->kAdminEmail, env->kPassword);
      ASSERT_FALSE(token.empty());
      const nlohmann::json req{{"name", "Provisioned Lab"}, {"contact", "p@p"}};
      const auto res = post("/api/v1/lab/create", req.dump(), token);
      EXPECT_EQ(res.status, 200) << res.raw;
      ASSERT_TRUE(res.body.is_object());
      EXPECT_FALSE(res.body["lab"].value("id", std::string{}).empty());
      EXPECT_EQ(res.body["lab"].value("name", std::string{}), "Provisioned Lab");
    }

    TEST(RestGatewayTest, CreateLabAsMemberReturns403) {
      auto* env = RestGatewayEnv::instance;
      const auto token = login(env->kMemberEmail, env->kPassword);
      ASSERT_FALSE(token.empty());
      const nlohmann::json req{{"name", "Nope"}, {"contact", "n@n"}};
      const auto res = post("/api/v1/lab/create", req.dump(), token);
      EXPECT_EQ(res.status, 403) << res.raw;
      EXPECT_EQ(res.body.value("code", std::string{}), "PERMISSION_DENIED");
    }

    TEST(RestGatewayTest, GetLabReturnsSeededLabForAdmin) {
      auto* env = RestGatewayEnv::instance;
      const auto token = login(env->kAdminEmail, env->kPassword);
      ASSERT_FALSE(token.empty());
      const nlohmann::json req{{"lab_id", env->kLabId}};
      const auto res = post("/api/v1/lab/get", req.dump(), token);
      EXPECT_EQ(res.status, 200) << res.raw;
      EXPECT_EQ(res.body["lab"].value("id", std::string{}), env->kLabId);
    }

    TEST(RestGatewayTest, GetLabRejectsMemberWithoutLabConfigure) {
      auto* env = RestGatewayEnv::instance;
      const auto token = login(env->kMemberEmail, env->kPassword);
      ASSERT_FALSE(token.empty());
      const nlohmann::json req{{"lab_id", env->kLabId}};
      const auto res = post("/api/v1/lab/get", req.dump(), token);
      EXPECT_EQ(res.status, 403) << res.raw;
    }

    // Proves the SampleService route is wired and gated. The seeded admin holds
    // sample.read in the lab; an empty list is the expected result.
    TEST(RestGatewayTest, ListSamplesForAdminReturns200) {
      auto* env = RestGatewayEnv::instance;
      const auto token = login(env->kAdminEmail, env->kPassword);
      ASSERT_FALSE(token.empty());
      const nlohmann::json req{{"lab_id", env->kLabId}};
      const auto res = post("/api/v1/sample/list", req.dump(), token);
      EXPECT_EQ(res.status, 200) << res.raw;
    }

    TEST(RestGatewayTest, ListSamplesWithoutBearerReturns401) {
      auto* env = RestGatewayEnv::instance;
      const nlohmann::json req{{"lab_id", env->kLabId}};
      const auto res = post("/api/v1/sample/list", req.dump());
      EXPECT_EQ(res.status, 401) << res.raw;
    }

    // E2E: login -> create lab -> read it back over REST only.
    TEST(RestGatewayTest, EndToEndCreateThenGetLab) {
      auto* env = RestGatewayEnv::instance;
      const auto token = login(env->kAdminEmail, env->kPassword);
      ASSERT_FALSE(token.empty());

      const nlohmann::json create{{"name", "E2E Lab"}, {"contact", "e2e@e2e"}};
      const auto created = post("/api/v1/lab/create", create.dump(), token);
      ASSERT_EQ(created.status, 200) << created.raw;
      const auto new_id = created.body["lab"].value("id", std::string{});
      ASSERT_FALSE(new_id.empty());

      const nlohmann::json get{{"lab_id", new_id}};
      const auto got = post("/api/v1/lab/get", get.dump(), token);
      EXPECT_EQ(got.status, 200) << got.raw;
      EXPECT_EQ(got.body["lab"].value("name", std::string{}), "E2E Lab");
    }

  } // namespace
} // namespace fmgr::test
