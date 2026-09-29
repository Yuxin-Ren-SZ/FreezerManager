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

#include <drogon/Cookie.h>
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
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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

      [[nodiscard]] std::uint16_t port() const {
        return port_;
      }

      void SetUp() override {
        instance = this;
        // The gateway reads the browser-session configuration when it registers
        // its routes, so the environment is set here rather than per test. Two
        // origins are acceptable to the CSRF gate: the request's own host (which
        // is what the dev proxy and a same-origin production deploy present) and
        // FMGR_WEB_ORIGIN.
        ::setenv("FMGR_WEB_ORIGIN", "https://spa.example.test", 1);

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
        ::unsetenv("FMGR_WEB_ORIGIN");
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
      // Set-Cookie values the gateway sent, keyed by cookie name. Drogon parses
      // each Set-Cookie header into a Cookie, so the tests assert on attributes
      // rather than on the header text.
      std::map<std::string, drogon::Cookie> cookies;
    };

    using Headers = std::vector<std::pair<std::string, std::string>>;

    // `path` and `json_body` are both `const std::string&` and therefore
    // swappable in principle; the call sites below all pass a literal path and a
    // serialized body, so the risk is a misspelled path, not a silent mix-up.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    [[nodiscard]] HttpResult post_with(const std::string& path, const std::string& json_body,
                                       const Headers& headers) {
      auto* env = RestGatewayEnv::instance;
      auto client = drogon::HttpClient::newHttpClient(env->base_url());
      auto req = drogon::HttpRequest::newHttpRequest();
      req->setMethod(drogon::Post);
      req->setPath(path);
      req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
      req->setBody(json_body);
      for (const auto& [name, value] : headers) {
        req->addHeader(name, value);
      }
      auto [result, resp] = client->sendRequest(req, 10.0);
      EXPECT_EQ(result, drogon::ReqResult::Ok);
      HttpResult out;
      out.status = resp ? resp->getStatusCode() : 0;
      out.raw = resp ? std::string(resp->getBody()) : std::string{};
      out.body = nlohmann::json::parse(out.raw, nullptr, /*allow_exceptions=*/false);
      if (resp != nullptr) {
        for (const auto& [name, cookie] : resp->getCookies()) {
          out.cookies.emplace(name, cookie);
        }
      }
      return out;
    }

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    [[nodiscard]] HttpResult post(const std::string& path, const std::string& json_body,
                                  const std::string& bearer = {}) {
      Headers headers;
      if (!bearer.empty()) {
        headers.emplace_back("Authorization", "Bearer " + bearer);
      }
      return post_with(path, json_body, headers);
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

    // ---- Browser session helper (G0.1) ----
    //
    // A browser has a cookie jar; drogon's HttpClient does not, so the tests
    // carry the two cookies themselves and put them on each request by hand.
    struct BrowserSession {
      std::string session; // fmgr_session value = the bearer token
      std::string csrf;    // fmgr_csrf value
      HttpResult login;

      [[nodiscard]] std::string cookie_header() const {
        return "fmgr_session=" + session + "; fmgr_csrf=" + csrf;
      }

      // The headers a browser sends for a same-origin call: both cookies, plus
      // the CSRF header that echoes fmgr_csrf.
      [[nodiscard]] Headers headers(bool with_csrf = true) const {
        Headers out{{"Cookie", cookie_header()}};
        if (with_csrf) {
          out.emplace_back("X-CSRF-Token", csrf);
        }
        return out;
      }

      [[nodiscard]] Headers cookie_only() const {
        return Headers{{"Cookie", cookie_header()}};
      }
    };

    [[nodiscard]] BrowserSession browser_login(const std::string& email,
                                               const std::string& password) {
      const nlohmann::json req{{"email", email}, {"password", password}};
      BrowserSession session;
      session.login = post_with("/api/v1/auth/browser/login", req.dump(), {});
      const auto session_cookie = session.login.cookies.find("fmgr_session");
      if (session_cookie != session.login.cookies.end()) {
        session.session = session_cookie->second.value();
      }
      const auto csrf_cookie = session.login.cookies.find("fmgr_csrf");
      if (csrf_cookie != session.login.cookies.end()) {
        session.csrf = csrf_cookie->second.value();
      }
      return session;
    }

    // AuthService.Login is rate limited per source IP (30 attempts, 5/s refill)
    // and this file logs in for most of its tests. A browser session is
    // reusable, so the browser tests share one per account; only the tests that
    // are *about* logging in or out mint their own.
    [[nodiscard]] const BrowserSession& cached_browser_session(const std::string& email,
                                                               const std::string& password) {
      static std::map<std::string, BrowserSession> cache;
      const auto it = cache.find(email);
      if (it != cache.end()) {
        return it->second;
      }
      return cache.emplace(email, browser_login(email, password)).first->second;
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
      // G0.1's browser login answers with the user id and no token. The field is
      // filled at the source (AuthServiceImpl::Login), so both login routes
      // carry it — a bearer client gets the same additive field.
      EXPECT_FALSE(res.body.value("user_id", std::string{}).empty());
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

    // G0.4 through the real HTTP stack: JSON -> proto -> ListSamples -> SQL LIKE,
    // and back. Proves the `query` field is reachable from a browser client.
    TEST(RestGatewayTest, ListSamplesQueryFiltersByNameAndBarcodeThroughRest) {
      auto* env = RestGatewayEnv::instance;
      const auto token = login(env->kAdminEmail, env->kPassword);
      ASSERT_FALSE(token.empty());

      const nlohmann::json item_type_req{{"lab_id", env->kLabId}, {"name", "searchable"}};
      const auto item_type_res = post("/api/v1/item-type/create", item_type_req.dump(), token);
      ASSERT_EQ(item_type_res.status, 200) << item_type_res.raw;
      const auto item_type_id = item_type_res.body["item_type"].value("id", std::string{});
      ASSERT_FALSE(item_type_id.empty());

      const auto create_sample = [&](const std::string& name, const std::string& barcode) {
        const nlohmann::json req{{"lab_id", env->kLabId},
                                 {"item_type_id", item_type_id},
                                 {"name", name},
                                 {"barcode", barcode}};
        const auto res = post("/api/v1/sample/create", req.dump(), token);
        ASSERT_EQ(res.status, 200) << res.raw;
      };
      create_sample("Alpha-1", "BC-0001");
      create_sample("beta-2", "BC-0002");
      create_sample("unrelated", "ZZ-9981");

      const auto samples_of = [](const nlohmann::json& body) {
        return body.contains("samples") ? body.at("samples") : nlohmann::json::array();
      };

      const nlohmann::json by_name{{"lab_id", env->kLabId}, {"query", "PHa"}};
      const auto name_res = post("/api/v1/sample/list", by_name.dump(), token);
      ASSERT_EQ(name_res.status, 200) << name_res.raw;
      const auto name_samples = samples_of(name_res.body);
      ASSERT_EQ(name_samples.size(), 1U) << name_res.raw;
      EXPECT_EQ(name_samples.at(0).value("name", std::string{}), "Alpha-1");

      const nlohmann::json by_barcode{{"lab_id", env->kLabId}, {"query", "9981"}};
      const auto barcode_res = post("/api/v1/sample/list", by_barcode.dump(), token);
      ASSERT_EQ(barcode_res.status, 200) << barcode_res.raw;
      const auto barcode_samples = samples_of(barcode_res.body);
      ASSERT_EQ(barcode_samples.size(), 1U) << barcode_res.raw;
      EXPECT_EQ(barcode_samples.at(0).value("barcode", std::string{}), "ZZ-9981");

      const nlohmann::json too_short{{"lab_id", env->kLabId}, {"query", "A"}};
      const auto short_res = post("/api/v1/sample/list", too_short.dump(), token);
      EXPECT_EQ(short_res.status, 400) << short_res.raw;
      EXPECT_EQ(short_res.body.value("code", std::string{}), "INVALID_ARGUMENT");
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

    // #69: the catalog reads are sample.read (the permission a Member holds), so
    // a Member's generated sample form can be built through the REST gateway too.
    // The admin creates the row; the Member must see it, not a 403.
    TEST(RestGatewayTest, ListItemTypesAsMemberReturns200) {
      auto* env = RestGatewayEnv::instance;
      const auto admin = login(env->kAdminEmail, env->kPassword);
      ASSERT_FALSE(admin.empty());
      {
        const nlohmann::json create_req{{"lab_id", env->kLabId}, {"name", "member-visible"}};
        const auto created = post("/api/v1/item-type/create", create_req.dump(), admin);
        ASSERT_EQ(created.status, 200) << created.raw;
      }

      const auto token = login(env->kMemberEmail, env->kPassword);
      ASSERT_FALSE(token.empty());
      const nlohmann::json req{{"lab_id", env->kLabId}};
      const auto res = post("/api/v1/item-type/list", req.dump(), token);
      ASSERT_EQ(res.status, 200) << res.raw;
      ASSERT_TRUE(res.body.is_object());
      ASSERT_TRUE(res.body.contains("item_types")) << res.raw;
      EXPECT_FALSE(res.body["item_types"].empty()) << res.raw;
    }

    // The negative direction at the REST layer: relaxing the catalog read must not
    // relax the write behind the same route family.
    TEST(RestGatewayTest, CreateItemTypeAsMemberReturns403) {
      auto* env = RestGatewayEnv::instance;
      const auto token = login(env->kMemberEmail, env->kPassword);
      ASSERT_FALSE(token.empty());
      const nlohmann::json req{{"lab_id", env->kLabId}, {"name", "hijack"}};
      const auto res = post("/api/v1/item-type/create", req.dump(), token);
      EXPECT_EQ(res.status, 403) << res.raw;
    }

    TEST(RestGatewayTest, ListCustomFieldDefinitionsAsMemberReturns200) {
      auto* env = RestGatewayEnv::instance;
      const auto token = login(env->kMemberEmail, env->kPassword);
      ASSERT_FALSE(token.empty());
      const nlohmann::json req{{"lab_id", env->kLabId}};
      const auto res = post("/api/v1/custom-field-def/list", req.dump(), token);
      EXPECT_EQ(res.status, 200) << res.raw;
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

    // ---- Browser session cookie + CSRF (G0.1) ----
    //
    // The SPA cannot hold a bearer token in JavaScript, so these routes put the
    // session in an HttpOnly cookie and guard cookie-authenticated mutations with
    // a double-submit CSRF token plus an Origin check.

    TEST(RestGatewayBrowserSession, LoginSetsBothCookiesAndReturnsNoToken) {
      auto* env = RestGatewayEnv::instance;
      const auto session = cached_browser_session(env->kAdminEmail, env->kPassword);
      ASSERT_EQ(session.login.status, 200) << session.login.raw;

      ASSERT_EQ(session.login.cookies.count("fmgr_session"), 1U);
      const auto& session_cookie = session.login.cookies.at("fmgr_session");
      EXPECT_FALSE(session_cookie.value().empty());
      EXPECT_EQ(session_cookie.value(), session.session);
      EXPECT_TRUE(session_cookie.isHttpOnly());
      EXPECT_TRUE(session_cookie.isSecure());
      EXPECT_EQ(session_cookie.sameSite(), drogon::Cookie::SameSite::kStrict);
      EXPECT_EQ(session_cookie.path(), "/api");
      // Server-side idle/absolute expiry are the real limits.
      EXPECT_FALSE(session_cookie.maxAge().has_value());

      ASSERT_EQ(session.login.cookies.count("fmgr_csrf"), 1U);
      const auto& csrf_cookie = session.login.cookies.at("fmgr_csrf");
      EXPECT_FALSE(csrf_cookie.value().empty());
      EXPECT_NE(csrf_cookie.value(), session_cookie.value());
      EXPECT_FALSE(csrf_cookie.isHttpOnly()); // JavaScript must be able to echo it
      EXPECT_TRUE(csrf_cookie.isSecure());
      EXPECT_EQ(csrf_cookie.sameSite(), drogon::Cookie::SameSite::kStrict);
      EXPECT_EQ(csrf_cookie.path(), "/");
      EXPECT_FALSE(csrf_cookie.maxAge().has_value());

      ASSERT_TRUE(session.login.body.is_object()) << session.login.raw;
      EXPECT_FALSE(session.login.body.value("session_id", std::string{}).empty());
      EXPECT_FALSE(session.login.body.value("user_id", std::string{}).empty());
      EXPECT_FALSE(session.login.body.value("mfa_required", false));
      // The whole point of the cookie: the token never reaches the body.
      EXPECT_FALSE(session.login.body.contains("session_token")) << session.login.raw;
    }

    TEST(RestGatewayBrowserSession, LoginWithWrongPasswordReturns401AndSetsNoCookies) {
      auto* env = RestGatewayEnv::instance;
      const auto session = browser_login(env->kAdminEmail, "definitely-wrong");
      EXPECT_EQ(session.login.status, 401) << session.login.raw;
      EXPECT_EQ(session.login.body.value("code", std::string{}), "UNAUTHENTICATED");
      EXPECT_TRUE(session.login.cookies.empty());
    }

    // A re-login while a stale session cookie is still in the jar: the SPA sends
    // both cookies, so the gate lets it through and the response rotates both.
    TEST(RestGatewayBrowserSession, LoginRotatesBothCookiesWhenAStaleSessionIsPresent) {
      auto* env = RestGatewayEnv::instance;
      const auto first = browser_login(env->kAdminEmail, env->kPassword);
      ASSERT_EQ(first.login.status, 200) << first.login.raw;
      ASSERT_FALSE(first.session.empty());

      const nlohmann::json req{{"email", env->kAdminEmail}, {"password", env->kPassword}};
      const auto second = post_with("/api/v1/auth/browser/login", req.dump(), first.headers());
      ASSERT_EQ(second.status, 200) << second.raw;
      ASSERT_EQ(second.cookies.count("fmgr_session"), 1U);
      ASSERT_EQ(second.cookies.count("fmgr_csrf"), 1U);
      EXPECT_NE(second.cookies.at("fmgr_session").value(), first.session);
      EXPECT_NE(second.cookies.at("fmgr_csrf").value(), first.csrf);
    }

    TEST(RestGatewayBrowserSession, SessionCookieAuthenticatesAUnaryRoute) {
      auto* env = RestGatewayEnv::instance;
      const auto session = cached_browser_session(env->kAdminEmail, env->kPassword);
      ASSERT_EQ(session.login.status, 200) << session.login.raw;

      const nlohmann::json req{{"lab_id", env->kLabId}};
      // No Authorization header: the credential is the cookie alone.
      const auto res = post_with("/api/v1/lab/get", req.dump(), session.headers());
      EXPECT_EQ(res.status, 200) << res.raw;
      EXPECT_EQ(res.body["lab"].value("id", std::string{}), env->kLabId);
    }

    // The cookie is a credential, not a privilege: the same RBAC gate answers.
    TEST(RestGatewayBrowserSession, SessionCookieRunsThroughTheSameRbacGate) {
      auto* env = RestGatewayEnv::instance;
      const auto member = cached_browser_session(env->kMemberEmail, env->kPassword);
      ASSERT_EQ(member.login.status, 200) << member.login.raw;

      const nlohmann::json list{{"lab_id", env->kLabId}};
      const auto allowed = post_with("/api/v1/sample/list", list.dump(), member.headers());
      EXPECT_EQ(allowed.status, 200) << allowed.raw;

      const nlohmann::json create{{"name", "Cookie Member Lab"}, {"contact", "c@c"}};
      const auto denied = post_with("/api/v1/lab/create", create.dump(), member.headers());
      EXPECT_EQ(denied.status, 403) << denied.raw;
      EXPECT_EQ(denied.body.value("code", std::string{}), "PERMISSION_DENIED");
    }

    TEST(RestGatewayBrowserSession, AuthorizationHeaderWinsOverTheSessionCookie) {
      auto* env = RestGatewayEnv::instance;
      const auto& admin = cached_browser_session(env->kAdminEmail, env->kPassword);
      ASSERT_EQ(admin.login.status, 200) << admin.login.raw;
      const auto& member = cached_browser_session(env->kMemberEmail, env->kPassword);
      ASSERT_FALSE(member.session.empty());

      // The member's session token doubles as a bearer token.
      auto headers = admin.headers();
      headers.emplace_back("Authorization", "Bearer " + member.session);

      // The admin cookie would allow this; the member bearer must not.
      const nlohmann::json create{{"name", "Header Wins Lab"}, {"contact", "h@h"}};
      const auto res = post_with("/api/v1/lab/create", create.dump(), headers);
      EXPECT_EQ(res.status, 403) << res.raw;
    }

    TEST(RestGatewayBrowserSession, CookiePostWithoutCsrfHeaderIsForbidden) {
      auto* env = RestGatewayEnv::instance;
      const auto session = cached_browser_session(env->kAdminEmail, env->kPassword);
      ASSERT_EQ(session.login.status, 200) << session.login.raw;

      const nlohmann::json req{{"name", "No Csrf Lab"}, {"contact", "n@n"}};
      const auto res = post_with("/api/v1/lab/create", req.dump(), session.cookie_only());
      EXPECT_EQ(res.status, 403) << res.raw;
      EXPECT_EQ(res.body.value("code", std::string{}), "PERMISSION_DENIED");
    }

    TEST(RestGatewayBrowserSession, CookiePostWithMismatchedCsrfHeaderIsForbidden) {
      auto* env = RestGatewayEnv::instance;
      const auto session = cached_browser_session(env->kAdminEmail, env->kPassword);
      ASSERT_EQ(session.login.status, 200) << session.login.raw;

      auto headers = session.cookie_only();
      headers.emplace_back("X-CSRF-Token", "not-the-cookie-value");

      const nlohmann::json req{{"name", "Bad Csrf Lab"}, {"contact", "b@b"}};
      const auto res = post_with("/api/v1/lab/create", req.dump(), headers);
      EXPECT_EQ(res.status, 403) << res.raw;
      EXPECT_EQ(res.body.value("code", std::string{}), "PERMISSION_DENIED");
    }

    // "Without calling gRPC" is only observable as "the mutation did not happen",
    // so this asks the server afterwards whether the lab exists.
    TEST(RestGatewayBrowserSession, CsrfRejectionNeverReachesGrpc) {
      auto* env = RestGatewayEnv::instance;
      const auto session = cached_browser_session(env->kAdminEmail, env->kPassword);
      ASSERT_EQ(session.login.status, 200) << session.login.raw;

      const std::string marker = "Csrf-Guard-Lab-Must-Not-Exist";
      const nlohmann::json create{{"name", marker}, {"contact", "guard@example.test"}};
      const auto denied = post_with("/api/v1/lab/create", create.dump(), session.cookie_only());
      ASSERT_EQ(denied.status, 403) << denied.raw;

      const auto labs = post_with("/api/v1/lab/list", "{}", session.headers());
      ASSERT_EQ(labs.status, 200) << labs.raw;
      EXPECT_EQ(labs.raw.find(marker), std::string::npos) << labs.raw;
    }

    TEST(RestGatewayBrowserSession, CookiePostWithForeignOriginIsForbidden) {
      auto* env = RestGatewayEnv::instance;
      const auto session = cached_browser_session(env->kAdminEmail, env->kPassword);
      ASSERT_EQ(session.login.status, 200) << session.login.raw;

      auto headers = session.headers();
      headers.emplace_back("Origin", "https://evil.example");

      const nlohmann::json req{{"name", "Foreign Origin Lab"}, {"contact", "f@f"}};
      const auto res = post_with("/api/v1/lab/create", req.dump(), headers);
      EXPECT_EQ(res.status, 403) << res.raw;
      EXPECT_EQ(res.body.value("code", std::string{}), "PERMISSION_DENIED");
    }

    // What a same-origin SPA presents: Origin == Host, including the dev port.
    TEST(RestGatewayBrowserSession, CookiePostWithTheRequestHostAsOriginSucceeds) {
      auto* env = RestGatewayEnv::instance;
      const auto session = cached_browser_session(env->kAdminEmail, env->kPassword);
      ASSERT_EQ(session.login.status, 200) << session.login.raw;

      auto headers = session.headers();
      headers.emplace_back("Origin", env->base_url());

      const nlohmann::json req{{"name", "Host Origin Lab"}, {"contact", "o@o"}};
      const auto res = post_with("/api/v1/lab/create", req.dump(), headers);
      EXPECT_EQ(res.status, 200) << res.raw;
    }

    // FMGR_WEB_ORIGIN (set in the test environment) is the escape hatch for a
    // deployment whose SPA lives on a different origin than the gateway.
    TEST(RestGatewayBrowserSession, CookiePostWithTheConfiguredWebOriginSucceeds) {
      auto* env = RestGatewayEnv::instance;
      const auto session = cached_browser_session(env->kAdminEmail, env->kPassword);
      ASSERT_EQ(session.login.status, 200) << session.login.raw;

      auto headers = session.headers();
      headers.emplace_back("Origin", "https://spa.example.test");

      const nlohmann::json req{{"name", "Web Origin Lab"}, {"contact", "w@w"}};
      const auto res = post_with("/api/v1/lab/create", req.dump(), headers);
      EXPECT_EQ(res.status, 200) << res.raw;
    }

    // Bearer calls carry no ambient credential: a script is unaffected by a
    // stale cookie or a foreign Origin.
    TEST(RestGatewayBrowserSession, BearerCallSkipsTheCsrfAndOriginGate) {
      auto* env = RestGatewayEnv::instance;
      const auto& session = cached_browser_session(env->kAdminEmail, env->kPassword);
      ASSERT_EQ(session.login.status, 200) << session.login.raw;

      Headers headers{{"Authorization", "Bearer " + session.session},
                      {"Cookie", session.cookie_header()},
                      {"Origin", "https://evil.example"}};

      const nlohmann::json req{{"name", "Bearer Gate Lab"}, {"contact", "g@g"}};
      const auto res = post_with("/api/v1/lab/create", req.dump(), headers);
      EXPECT_EQ(res.status, 200) << res.raw;
    }

    TEST(RestGatewayBrowserSession, UnknownSessionCookieIsUnauthenticated) {
      auto* env = RestGatewayEnv::instance;
      const nlohmann::json req{{"lab_id", env->kLabId}};
      const Headers headers{{"Cookie", "fmgr_session=not-a-real-token; fmgr_csrf=c"},
                            {"X-CSRF-Token", "c"}};
      const auto res = post_with("/api/v1/lab/get", req.dump(), headers);
      EXPECT_EQ(res.status, 401) << res.raw;
      EXPECT_EQ(res.body.value("code", std::string{}), "UNAUTHENTICATED");
    }

    TEST(RestGatewayBrowserSession, LogoutRevokesTheSessionAndExpiresBothCookies) {
      auto* env = RestGatewayEnv::instance;
      const auto session = browser_login(env->kAdminEmail, env->kPassword);
      ASSERT_EQ(session.login.status, 200) << session.login.raw;

      const nlohmann::json req{{"lab_id", env->kLabId}};
      ASSERT_EQ(post_with("/api/v1/lab/get", req.dump(), session.headers()).status, 200);

      const auto logout = post_with("/api/v1/auth/browser/logout", "{}", session.headers());
      ASSERT_EQ(logout.status, 200) << logout.raw;

      ASSERT_EQ(logout.cookies.count("fmgr_session"), 1U);
      const auto& session_cookie = logout.cookies.at("fmgr_session");
      EXPECT_TRUE(session_cookie.value().empty());
      EXPECT_EQ(session_cookie.maxAge(), std::optional<int>{0});
      EXPECT_EQ(session_cookie.path(), "/api");
      EXPECT_TRUE(session_cookie.isHttpOnly());
      EXPECT_TRUE(session_cookie.isSecure());

      ASSERT_EQ(logout.cookies.count("fmgr_csrf"), 1U);
      const auto& csrf_cookie = logout.cookies.at("fmgr_csrf");
      EXPECT_TRUE(csrf_cookie.value().empty());
      EXPECT_EQ(csrf_cookie.maxAge(), std::optional<int>{0});
      EXPECT_EQ(csrf_cookie.path(), "/");
      EXPECT_FALSE(csrf_cookie.isHttpOnly());

      // Revoked server-side, not merely dropped client-side.
      const auto after = post_with("/api/v1/lab/get", req.dump(), session.headers());
      EXPECT_EQ(after.status, 401) << after.raw;
      EXPECT_EQ(after.body.value("code", std::string{}), "UNAUTHENTICATED");
    }

    TEST(RestGatewayBrowserSession, SubmitMfaIsCookieAuthenticatedAndCsrfGuarded) {
      auto* env = RestGatewayEnv::instance;
      const auto session = cached_browser_session(env->kAdminEmail, env->kPassword);
      ASSERT_EQ(session.login.status, 200) << session.login.raw;

      const nlohmann::json req{{"totp_code", "123456"}};

      const auto guarded =
          post_with("/api/v1/auth/browser/submit-mfa", req.dump(), session.cookie_only());
      EXPECT_EQ(guarded.status, 403) << guarded.raw;

      // With the CSRF header the RPC runs; the seeded admin has no TOTP secret,
      // so the answer is the handler's InvalidCredentials, not a missing route.
      const auto reached =
          post_with("/api/v1/auth/browser/submit-mfa", req.dump(), session.headers());
      EXPECT_EQ(reached.status, 401) << reached.raw;
      EXPECT_EQ(reached.body.value("code", std::string{}), "UNAUTHENTICATED");
    }

    TEST(RestGatewayBrowserSession, SubmitMfaWithoutACookieIsUnauthenticated) {
      const nlohmann::json req{{"totp_code", "123456"}};
      const auto res = post_with("/api/v1/auth/browser/submit-mfa", req.dump(), {});
      EXPECT_EQ(res.status, 401) << res.raw;
    }

    // ---- SSE helper ----
    //
    // drogon::HttpClient waits for a complete response, which never arrives on an
    // open SSE stream. So drive the feed over a raw socket: send the GET, fire a
    // trigger (a mutation that appends an audit row) once the stream is up, and
    // accumulate bytes until `needle` appears or the deadline passes.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    [[nodiscard]] std::string sse_read_until_headers(const std::string& path,
                                                     const Headers& headers,
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
      std::string request;
      request.reserve(path.size() + 64);
      request += "GET ";
      request += path;
      request += " HTTP/1.1\r\nHost: 127.0.0.1\r\n";
      for (const auto& [name, value] : headers) {
        request += name;
        request += ": ";
        request += value;
        request += "\r\n";
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

    // Bearer variant kept for the tests that predate the browser session.
    //
    // `path` and `bearer` are adjacent `const std::string&` parameters and so are
    // swappable in principle. The call sites pass a `/api/v1/...` path and a token,
    // and a mix-up would fail the read rather than pass it: the path is what the
    // server routes on, and a token there never matches a route.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    [[nodiscard]] std::string sse_read_until(const std::string& path, const std::string& bearer,
                                             const std::string& needle,
                                             const std::function<void()>& trigger,
                                             double timeout_s) {
      Headers headers;
      if (!bearer.empty()) {
        headers.emplace_back("Authorization", "Bearer " + bearer);
      }
      return sse_read_until_headers(path, headers, needle, trigger, timeout_s);
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

    // Positive: the same feed authenticates from the fmgr_session cookie, which
    // is the only credential a browser EventSource can carry — it cannot set an
    // Authorization header, and G0.1 removed the `?access_token=` workaround that
    // used to put the token in the URL (and therefore in every access log).
    TEST(RestGatewaySse, SampleWatchStreamsUsingTheSessionCookie) {
      auto* env = RestGatewayEnv::instance;
      const auto token = login(env->kAdminEmail, env->kPassword);
      ASSERT_FALSE(token.empty());
      const auto session = browser_login(env->kAdminEmail, env->kPassword);
      ASSERT_EQ(session.login.status, 200) << session.login.raw;

      const nlohmann::json it_req{{"lab_id", env->kLabId}, {"name", "sse-cookie-itemtype"}};
      const auto it_res = post("/api/v1/item-type/create", it_req.dump(), token);
      ASSERT_EQ(it_res.status, 200) << it_res.raw;
      const auto item_type_id = it_res.body["item_type"].value("id", std::string{});
      ASSERT_FALSE(item_type_id.empty());

      const std::string sample_name = "SSE Cookie Sample Alpha";
      const auto trigger = [&] {
        const nlohmann::json req{
            {"lab_id", env->kLabId}, {"item_type_id", item_type_id}, {"name", sample_name}};
        (void)post("/api/v1/sample/create", req.dump(), token);
      };

      const std::string out =
          sse_read_until_headers("/api/v1/sample/watch?lab_id=" + env->kLabId,
                                 session.cookie_only(), sample_name, trigger, 12.0);
      EXPECT_NE(out.find("text/event-stream"), std::string::npos) << out.substr(0, 200);
      EXPECT_NE(out.find("data:"), std::string::npos) << out.substr(0, 400);
      EXPECT_NE(out.find(sample_name), std::string::npos) << out.substr(0, 400);
    }

    // The removed fallback: a valid token in the query string no longer
    // authenticates, so the gRPC gate rejects at stream-open and the failure
    // arrives as an `event: error` frame (the status is already committed).
    TEST(RestGatewaySse, AuditWatchRejectsTheAccessTokenQueryParameter) {
      auto* env = RestGatewayEnv::instance;
      const auto token = login(env->kAdminEmail, env->kPassword);
      ASSERT_FALSE(token.empty());

      const std::string out = sse_read_until_headers("/api/v1/audit/watch?access_token=" + token,
                                                     {}, "event: error", nullptr, 8.0);
      EXPECT_NE(out.find("event: error"), std::string::npos) << out.substr(0, 400);
      EXPECT_NE(out.find("UNAUTHENTICATED"), std::string::npos) << out.substr(0, 400);
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
