// SPDX-License-Identifier: AGPL-3.0-or-later

// End-to-end tests for the REST/JSON gateway. A single Drogon app (process
// singleton) is stood up once via a global test environment: it fronts a real
// in-process FreezerServer over the gRPC in-process channel, so these tests
// exercise the full path JSON -> proto -> RBAC gate -> repo/audit/txn -> JSON.
//
// Per PRD §15 every exposed endpoint gets a positive and a negative
// authorization test (a caller who may, one who may not, plus missing bearer).

#include "auth/LocalAuthProvider.h"
#include "support/FastAuth.h"
#include "support/RegisterRepositories.h"
#include "support/TempSqliteDb.h"
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
#include "storage/sqlite/LoginAttemptRepositories.h"
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

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>

namespace fmgr::test {
  namespace {


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

      [[nodiscard]] std::uint16_t port() const {
        return port_;
      }

      void SetUp() override {
        instance = this;
        db_ = std::make_unique<TempSqliteDb>("fmgr-rest-it");

        backend_ = std::make_unique<storage::SqliteBackend>(
            storage::SqliteBackendOptions{.database_path = db_->string()});
        register_all_sqlite_repositories(*backend_);
        backend_->migrate_to_latest();
        provider_ = std::make_unique<auth::LocalAuthProvider>(*backend_, fast_auth_config());
        seed();

        server_opts_.listen_address = "localhost:0";
        server_ = std::make_unique<server::FreezerServer>(*backend_, *provider_, server_opts_);
        server_->build();

        stubs_ = std::make_unique<rest::GatewayStubs>(server_->in_process_channel());
        gateway_ = std::make_unique<rest::RestGateway>(*stubs_);
        gateway_->register_routes();
        // Health probe wired to the real backend (begin/rollback reachability);
        // KMS + backup report disabled in this harness. Exercises the route end
        // to end without bringing up a KMS or backup target.
        storage::IStorageBackend* backend_ptr = backend_.get();
        gateway_->register_health(obs::HealthProbe{
            .database =
                [backend_ptr] {
                  try {
                    auto txn = backend_ptr->begin(storage::IsolationLevel::ReadCommitted);
                    txn->rollback();
                    return obs::DepStatus::ok();
                  } catch (const std::exception& e) {
                    return obs::DepStatus::failed(e.what());
                  }
                },
            .kms = [] { return obs::DepStatus::disabled("no KEK in test"); },
            .backup = [] { return obs::DepStatus::disabled("no backup dir in test"); },
        });
        gateway_->register_liveness();
        gateway_->register_metrics();

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
        db_.reset();
      }

    private:


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

      std::unique_ptr<TempSqliteDb> db_;
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

    [[nodiscard]] HttpResult get(const std::string& path) {
      auto* env = RestGatewayEnv::instance;
      auto client = drogon::HttpClient::newHttpClient(env->base_url());
      auto req = drogon::HttpRequest::newHttpRequest();
      req->setMethod(drogon::Get);
      req->setPath(path);
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

    // ---- Fan-out services: Box / ItemType / Role / Audit / Share ----
    //
    // Each fan-out service gets a positive (caller who holds the permission),
    // a negative (caller who does not -> 403), and a missing-bearer (401) check,
    // per PRD §15. The seeded admin is a SystemAdmin (holds every built-in
    // permission); the seeded member holds only sample.* + share.request.

    // -- BoxService --
    TEST(RestGatewayTest, ListFreezersForAdminReturns200) {
      auto* env = RestGatewayEnv::instance;
      const auto token = login(env->kAdminEmail, env->kPassword);
      ASSERT_FALSE(token.empty());
      const nlohmann::json req{{"lab_id", env->kLabId}};
      const auto res = post("/api/v1/freezer/list", req.dump(), token);
      EXPECT_EQ(res.status, 200) << res.raw;
    }

    TEST(RestGatewayTest, CreateFreezerAsMemberReturns403) {
      auto* env = RestGatewayEnv::instance;
      const auto token = login(env->kMemberEmail, env->kPassword);
      ASSERT_FALSE(token.empty());
      const nlohmann::json req{{"lab_id", env->kLabId}, {"name", "F1"}};
      const auto res = post("/api/v1/freezer/create", req.dump(), token);
      EXPECT_EQ(res.status, 403) << res.raw;
      EXPECT_EQ(res.body.value("code", std::string{}), "PERMISSION_DENIED");
    }

    TEST(RestGatewayTest, ListFreezersWithoutBearerReturns401) {
      auto* env = RestGatewayEnv::instance;
      const nlohmann::json req{{"lab_id", env->kLabId}};
      const auto res = post("/api/v1/freezer/list", req.dump());
      EXPECT_EQ(res.status, 401) << res.raw;
    }

    // -- ItemTypeService --
    TEST(RestGatewayTest, ListItemTypesForAdminReturns200) {
      auto* env = RestGatewayEnv::instance;
      const auto token = login(env->kAdminEmail, env->kPassword);
      ASSERT_FALSE(token.empty());
      const nlohmann::json req{{"lab_id", env->kLabId}};
      const auto res = post("/api/v1/item-type/list", req.dump(), token);
      EXPECT_EQ(res.status, 200) << res.raw;
    }

    // E2E create through a fan-out service: proves JSON -> proto -> handler ->
    // repo + audit append -> commit -> JSON round-trips for a write RPC.
    TEST(RestGatewayTest, CreateItemTypeAsAdminSucceeds) {
      auto* env = RestGatewayEnv::instance;
      const auto token = login(env->kAdminEmail, env->kPassword);
      ASSERT_FALSE(token.empty());
      const nlohmann::json req{{"lab_id", env->kLabId}, {"name", "liquid"}};
      const auto res = post("/api/v1/item-type/create", req.dump(), token);
      EXPECT_EQ(res.status, 200) << res.raw;
      ASSERT_TRUE(res.body.is_object());
      EXPECT_FALSE(res.body["item_type"].value("id", std::string{}).empty());
      EXPECT_EQ(res.body["item_type"].value("name", std::string{}), "liquid");
    }

    TEST(RestGatewayTest, ListItemTypesAsMemberReturns403) {
      auto* env = RestGatewayEnv::instance;
      const auto token = login(env->kMemberEmail, env->kPassword);
      ASSERT_FALSE(token.empty());
      const nlohmann::json req{{"lab_id", env->kLabId}};
      const auto res = post("/api/v1/item-type/list", req.dump(), token);
      EXPECT_EQ(res.status, 403) << res.raw;
    }

    // List has lenient request validation, so the missing-bearer path reaches
    // the token check (write RPCs validate the body first and would 400 instead).
    TEST(RestGatewayTest, ItemTypeListWithoutBearerReturns401) {
      auto* env = RestGatewayEnv::instance;
      const nlohmann::json req{{"lab_id", env->kLabId}};
      const auto res = post("/api/v1/item-type/list", req.dump());
      EXPECT_EQ(res.status, 401) << res.raw;
    }

    // -- RoleService --
    TEST(RestGatewayTest, ListRolesForAdminReturns200) {
      auto* env = RestGatewayEnv::instance;
      const auto token = login(env->kAdminEmail, env->kPassword);
      ASSERT_FALSE(token.empty());
      const nlohmann::json req{{"lab_id", env->kLabId}};
      const auto res = post("/api/v1/role/list", req.dump(), token);
      EXPECT_EQ(res.status, 200) << res.raw;
    }

    TEST(RestGatewayTest, ListRolesAsMemberReturns403) {
      auto* env = RestGatewayEnv::instance;
      const auto token = login(env->kMemberEmail, env->kPassword);
      ASSERT_FALSE(token.empty());
      const nlohmann::json req{{"lab_id", env->kLabId}};
      const auto res = post("/api/v1/role/list", req.dump(), token);
      EXPECT_EQ(res.status, 403) << res.raw;
    }

    TEST(RestGatewayTest, RoleListWithoutBearerReturns401) {
      auto* env = RestGatewayEnv::instance;
      const nlohmann::json req{{"lab_id", env->kLabId}};
      const auto res = post("/api/v1/role/list", req.dump());
      EXPECT_EQ(res.status, 401) << res.raw;
    }

    // -- AuditService --
    TEST(RestGatewayTest, ListAuditEventsForAdminReturns200) {
      auto* env = RestGatewayEnv::instance;
      const auto token = login(env->kAdminEmail, env->kPassword);
      ASSERT_FALSE(token.empty());
      const nlohmann::json req{{"lab_id", env->kLabId}};
      const auto res = post("/api/v1/audit/list", req.dump(), token);
      EXPECT_EQ(res.status, 200) << res.raw;
    }

    TEST(RestGatewayTest, ListAuditEventsAsMemberReturns403) {
      auto* env = RestGatewayEnv::instance;
      const auto token = login(env->kMemberEmail, env->kPassword);
      ASSERT_FALSE(token.empty());
      const nlohmann::json req{{"lab_id", env->kLabId}};
      const auto res = post("/api/v1/audit/list", req.dump(), token);
      EXPECT_EQ(res.status, 403) << res.raw;
    }

    TEST(RestGatewayTest, ExportAuditLogWithoutBearerReturns401) {
      const auto res = post("/api/v1/audit/export", "{}");
      EXPECT_EQ(res.status, 401) << res.raw;
    }

    // -- ShareService --
    TEST(RestGatewayTest, ListShareRequestsForAdminReturns200) {
      auto* env = RestGatewayEnv::instance;
      const auto token = login(env->kAdminEmail, env->kPassword);
      ASSERT_FALSE(token.empty());
      const nlohmann::json req{{"source_lab_id", env->kLabId}};
      const auto res = post("/api/v1/share/list", req.dump(), token);
      EXPECT_EQ(res.status, 200) << res.raw;
    }

    // ShareService share.approve authz is exercised at the gRPC layer in
    // share_service_integration_test.cpp (ApproveShareRequest validates the
    // approver_role and resolves the request before the role gate, so a clean
    // REST permission-denied negative would need a full pending-request fixture).
    // Here the REST surface is covered by the admin-200 and missing-bearer-401
    // checks plus the create authz path below.
    TEST(RestGatewayTest, CreateShareRequestWithoutBearerReturns401) {
      auto* env = RestGatewayEnv::instance;
      const nlohmann::json req{{"source_lab_id", env->kLabId},
                               {"target_lab_id", "20000000-0000-0000-0000-000000000002"},
                               {"scope_json", "{}"}};
      const auto res = post("/api/v1/share/create", req.dump());
      EXPECT_EQ(res.status, 401) << res.raw;
    }

    TEST(RestGatewayTest, ListShareRequestsWithoutBearerReturns401) {
      auto* env = RestGatewayEnv::instance;
      const nlohmann::json req{{"source_lab_id", env->kLabId}};
      const auto res = post("/api/v1/share/list", req.dump());
      EXPECT_EQ(res.status, 401) << res.raw;
    }

    // ---- SSE helper ----
    //
    // drogon::HttpClient waits for a complete response, which never arrives on an
    // open SSE stream. So drive the feed over a raw socket: send the GET, fire a
    // trigger (a mutation that appends an audit row) once the stream is up, and
    // accumulate bytes until `needle` appears or the deadline passes.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    [[nodiscard]] std::string sse_read_until(const std::string& path, const std::string& bearer,
                                             const std::string& needle,
                                             const std::function<void()>& trigger,
                                             double timeout_s) {
      auto* env = RestGatewayEnv::instance;
      const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
      if (fd < 0) {
        return {};
      }
      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(env->port());
      addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return {};
      }
      std::string request = "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\n";
      if (!bearer.empty()) {
        request += "Authorization: Bearer " + bearer + "\r\n";
      }
      request += "Accept: text/event-stream\r\nConnection: keep-alive\r\n\r\n";
      (void)::send(fd, request.data(), request.size(), 0);

      timeval recv_timeout{.tv_sec = 0, .tv_usec = 500000};
      (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &recv_timeout, sizeof(recv_timeout));

      std::thread trig([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        if (trigger) {
          trigger();
        }
      });

      std::string acc;
      std::array<char, 4096> buf{};
      const auto deadline =
          std::chrono::steady_clock::now() + std::chrono::duration<double>(timeout_s);
      while (std::chrono::steady_clock::now() < deadline) {
        const ssize_t n = ::recv(fd, buf.data(), buf.size(), 0);
        if (n > 0) {
          acc.append(buf.data(), static_cast<std::size_t>(n));
          if (acc.find(needle) != std::string::npos) {
            break;
          }
        } else if (n == 0) {
          break; // peer closed
        }
        // n < 0 → recv timeout; keep polling until the overall deadline.
      }
      trig.join();
      ::close(fd);
      return acc;
    }

    // Positive: an unscoped feed (SystemAdmin) streams a freshly-appended audit
    // row. The created lab's name lands in the event's after_json, so it appears
    // in the SSE `data:` frame.
    TEST(RestGatewaySse, AuditWatchStreamsNewEvent) {
      auto* env = RestGatewayEnv::instance;
      const auto token = login(env->kAdminEmail, env->kPassword);
      ASSERT_FALSE(token.empty());

      const std::string lab_name = "SSE Watch Lab Alpha";
      const auto trigger = [&] {
        const nlohmann::json req{{"name", lab_name}, {"contact", "s@s"}};
        (void)post("/api/v1/lab/create", req.dump(), token);
      };

      const std::string out = sse_read_until("/api/v1/audit/watch", token, lab_name, trigger, 12.0);
      EXPECT_NE(out.find("text/event-stream"), std::string::npos) << out.substr(0, 200);
      EXPECT_NE(out.find("data:"), std::string::npos) << out.substr(0, 400);
      EXPECT_NE(out.find(lab_name), std::string::npos);
    }

    // Negative: without a bearer the gRPC gate rejects at stream-open. Because the
    // SSE response status is already committed, the failure surfaces as an
    // `event: error` frame carrying the gRPC code rather than an HTTP 401.
    TEST(RestGatewaySse, AuditWatchWithoutBearerStreamsErrorEvent) {
      const std::string out =
          sse_read_until("/api/v1/audit/watch", "", "event: error", nullptr, 8.0);
      EXPECT_NE(out.find("event: error"), std::string::npos) << out.substr(0, 400);
      EXPECT_NE(out.find("UNAUTHENTICATED"), std::string::npos);
    }

    // Positive: the lab-scoped sample feed streams a freshly-created sample. The
    // sample's name lands in the proto-JSON `data:` frame.
    TEST(RestGatewaySse, SampleWatchStreamsNewSample) {
      auto* env = RestGatewayEnv::instance;
      const auto token = login(env->kAdminEmail, env->kPassword);
      ASSERT_FALSE(token.empty());

      // A sample needs a live item type; create one up front (before the stream).
      // Unique name — the gateway test env is a process-shared singleton DB and
      // other tests create their own item types in the same lab.
      const nlohmann::json it_req{{"lab_id", env->kLabId}, {"name", "sse-watch-itemtype"}};
      const auto it_res = post("/api/v1/item-type/create", it_req.dump(), token);
      ASSERT_EQ(it_res.status, 200) << it_res.raw;
      const auto item_type_id = it_res.body["item_type"].value("id", std::string{});
      ASSERT_FALSE(item_type_id.empty());

      const std::string sample_name = "SSE Watch Sample Alpha";
      const auto trigger = [&] {
        const nlohmann::json req{
            {"lab_id", env->kLabId}, {"item_type_id", item_type_id}, {"name", sample_name}};
        (void)post("/api/v1/sample/create", req.dump(), token);
      };

      const std::string out = sse_read_until("/api/v1/sample/watch?lab_id=" + env->kLabId, token,
                                             sample_name, trigger, 12.0);
      EXPECT_NE(out.find("text/event-stream"), std::string::npos) << out.substr(0, 200);
      EXPECT_NE(out.find("data:"), std::string::npos) << out.substr(0, 400);
      EXPECT_NE(out.find(sample_name), std::string::npos);
    }

    // Negative: without a bearer the gRPC gate rejects at stream-open; the
    // failure surfaces as an `event: error` frame (status already committed).
    TEST(RestGatewaySse, SampleWatchWithoutBearerStreamsErrorEvent) {
      auto* env = RestGatewayEnv::instance;
      const std::string out = sse_read_until("/api/v1/sample/watch?lab_id=" + env->kLabId, "",
                                             "event: error", nullptr, 8.0);
      EXPECT_NE(out.find("event: error"), std::string::npos) << out.substr(0, 400);
      EXPECT_NE(out.find("UNAUTHENTICATED"), std::string::npos);
    }

    // ---- /health (PRD §17) ----

    // Unauthenticated readiness probe: 200 with a per-dependency report when the
    // database is reachable. KMS + backup are disabled in this harness, which does
    // not fail the verdict.
    TEST(RestGatewayHealth, HealthReturns200WithPerDependencyReport) {
      const auto res = get("/api/v1/health");
      ASSERT_EQ(res.status, 200) << res.raw;
      ASSERT_TRUE(res.body.is_object()) << res.raw;
      EXPECT_EQ(res.body.at("status"), "ok");
      EXPECT_EQ(res.body.at("checks").at("database").at("status"), "ok");
      EXPECT_EQ(res.body.at("checks").at("kms").at("status"), "disabled");
      EXPECT_EQ(res.body.at("checks").at("backup").at("status"), "disabled");
    }

    // The /healthz alias serves the same probe (k8s/LB convention).
    TEST(RestGatewayHealth, HealthzAliasReturns200) {
      const auto res = get("/healthz");
      EXPECT_EQ(res.status, 200) << res.raw;
      EXPECT_EQ(res.body.at("status"), "ok");
    }

    // No bearer required — the probe is reachable without authentication.
    TEST(RestGatewayHealth, HealthNeedsNoBearer) {
      const auto res = get("/api/v1/health");
      EXPECT_EQ(res.status, 200) << res.raw;
    }

    // ---- /metrics (PRD §17) ----

    // The Prometheus endpoint is unauthenticated and serves text exposition with
    // the expected content type.
    TEST(RestGatewayMetrics, MetricsReturnsPrometheusText) {
      auto* env = RestGatewayEnv::instance;
      auto client = drogon::HttpClient::newHttpClient(env->base_url());
      auto req = drogon::HttpRequest::newHttpRequest();
      req->setMethod(drogon::Get);
      req->setPath("/metrics");
      auto [result, resp] = client->sendRequest(req, 10.0);
      ASSERT_EQ(result, drogon::ReqResult::Ok);
      ASSERT_EQ(resp->getStatusCode(), 200);
      EXPECT_NE(resp->getHeader("content-type").find("text/plain"), std::string::npos);
    }

    // Driving an RPC through the gateway increments the gRPC interceptor's
    // per-method counter, observable on the next scrape.
    TEST(RestGatewayMetrics, RpcCounterIncrementsAfterCall) {
      auto* env = RestGatewayEnv::instance;
      const auto token = login(env->kAdminEmail, env->kPassword);
      ASSERT_FALSE(token.empty());
      const nlohmann::json body{{"name", "Metrics Lab"}, {"contact", "pi@metrics.example"}};
      ASSERT_EQ(post("/api/v1/lab/create", body.dump(), token).status, 200);

      const auto res = get("/metrics");
      ASSERT_EQ(res.status, 200);
      EXPECT_NE(res.raw.find("rpc_requests_total"), std::string::npos) << res.raw;
      EXPECT_NE(res.raw.find("/fmgr.v1.LabService/CreateLab"), std::string::npos) << res.raw;
      EXPECT_NE(res.raw.find("rpc_latency_seconds_bucket"), std::string::npos) << res.raw;
    }

  } // namespace
} // namespace fmgr::test
