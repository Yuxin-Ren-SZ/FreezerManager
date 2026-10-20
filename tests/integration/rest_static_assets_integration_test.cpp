// SPDX-License-Identifier: AGPL-3.0-or-later

// End-to-end tests for freezerd serving the built SPA (TODO.md G0.3): static
// files, the client-side deep-link fallback, the JSON 404 for unknown API
// paths, traversal refusal, browser security/cache headers and the raised
// request-body limit.
//
// The server here is Drogon alone plus a few stand-in handlers for the gateway
// routes whose precedence matters; the real gateway is exercised by
// rest_gateway_integration_test.cpp. This binary runs the common configuration:
// FMGR_WEB_ROOT set, plaintext listener, default (10 MiB) body limit.

#include "rest/StaticAssets.h"
#include "rest_static_assets_harness.h"

#include <drogon/drogon.h>
#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdlib>
#include <functional>
#include <string>
#include <thread>
#include <utility>

namespace fmgr::test {
  namespace {

    using static_assets::find_free_port;
    using static_assets::get;
    using static_assets::HttpResult;
    using static_assets::kHashedAsset;
    using static_assets::kIndexHtml;
    using static_assets::post;
    using static_assets::wait_until_running;
    using static_assets::WebRoot;

    constexpr std::string_view kExpectedCsp =
        "default-src 'self'; frame-ancestors 'none'; base-uri 'none'; object-src 'none'";

    class RestStaticAssetsEnv : public ::testing::Environment {
    public:
      static RestStaticAssetsEnv* instance;

      void SetUp() override {
        instance = this;
        // The developer's shell must not decide what this binary tests.
        ::unsetenv("FMGR_WEB_ROOT");
        ::unsetenv("FMGR_REST_MAX_BODY_BYTES");
        web_ = WebRoot::create("plaintext");
        ::setenv("FMGR_WEB_ROOT", web_.root.string().c_str(), 1);

        const auto config = rest::StaticAssetsConfig::from_env(/*tls=*/false);
        if (!config.serves_spa()) {
          ADD_FAILURE() << "FMGR_WEB_ROOT did not enable static serving";
          return;
        }

        // Stand-ins for the real gateway routes: exact handlers must keep
        // winning over the SPA fallback.
        drogon::app().registerHandler(
            "/api/v1/sample/import",
            [](const drogon::HttpRequestPtr& req,
               std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
              auto resp = drogon::HttpResponse::newHttpResponse();
              resp->setStatusCode(drogon::k200OK);
              resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
              resp->setBody(nlohmann::json{{"received", req->body().size()}}.dump());
              callback(resp);
            },
            {drogon::Post});
        drogon::app().registerHandler(
            "/healthz",
            [](const drogon::HttpRequestPtr&,
               std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
              auto resp = drogon::HttpResponse::newHttpResponse();
              resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
              resp->setBody(R"({"status":"ok"})");
              callback(resp);
            },
            {drogon::Get});
        drogon::app().registerHandler(
            "/metrics",
            [](const drogon::HttpRequestPtr&,
               std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
              auto resp = drogon::HttpResponse::newHttpResponse();
              resp->setContentTypeString("text/plain; version=0.0.4; charset=utf-8");
              resp->setBody("# HELP fmgr_test_metric test\nfmgr_test_metric 1\n");
              callback(resp);
            },
            {drogon::Get});

        rest::StaticAssets::install(config);

        port_ = find_free_port();
        drogon::app().addListener("127.0.0.1", port_);
        drogon::app().setThreadNum(1);
        started_ = true;
        app_thread_ = std::thread([] { drogon::app().run(); });
        wait_until_running();
      }

      void TearDown() override {
        if (started_) {
          drogon::app().quit();
          if (app_thread_.joinable()) {
            app_thread_.join();
          }
        }
        web_.remove();
      }

      [[nodiscard]] std::string base_url() const {
        return "http://127.0.0.1:" + std::to_string(port_);
      }

      [[nodiscard]] const WebRoot& web() const {
        return web_;
      }

    private:
      WebRoot web_;
      std::uint16_t port_{0};
      std::thread app_thread_;
      bool started_{false};
    };

    RestStaticAssetsEnv* RestStaticAssetsEnv::instance = nullptr;

    const ::testing::Environment* const kEnv =
        ::testing::AddGlobalTestEnvironment(new RestStaticAssetsEnv);

    [[nodiscard]] std::string base_url() {
      return RestStaticAssetsEnv::instance->base_url();
    }

    [[nodiscard]] HttpResult fetch(const std::string& path) {
      return get(base_url(), path);
    }

    TEST(RestStaticAssets, ServesIndexHtmlAtTheRoot) {
      const auto result = fetch("/");
      ASSERT_TRUE(result.ok());
      EXPECT_EQ(result.status, 200);
      EXPECT_EQ(result.body, kIndexHtml);
      EXPECT_NE(result.content_type.find("text/html"), std::string::npos) << result.content_type;
      EXPECT_EQ(result.cache_control, "no-cache");
    }

    TEST(RestStaticAssets, ServesIndexHtmlForAClientSideDeepLink) {
      const auto result = fetch("/labs/20000000-0000-0000-0000-000000000001/samples/"
                                "30000000-0000-0000-0000-000000000002");
      ASSERT_TRUE(result.ok());
      EXPECT_EQ(result.status, 200);
      EXPECT_EQ(result.body, kIndexHtml);
      EXPECT_EQ(result.cache_control, "no-cache");
    }

    TEST(RestStaticAssets, ServesAHashedAssetFromTheWebRoot) {
      const auto result = fetch("/assets/app-abc123.js");
      ASSERT_TRUE(result.ok());
      EXPECT_EQ(result.status, 200);
      EXPECT_EQ(result.body, kHashedAsset);
      EXPECT_NE(result.content_type.find("javascript"), std::string::npos) << result.content_type;
      EXPECT_NE(result.cache_control.find("immutable"), std::string::npos) << result.cache_control;
      EXPECT_NE(result.cache_control.find("max-age=31536000"), std::string::npos)
          << result.cache_control;
    }

    TEST(RestStaticAssets, UnknownApiPathGetsAJson404) {
      const auto result = fetch("/api/v1/does-not-exist");
      ASSERT_TRUE(result.ok());
      EXPECT_EQ(result.status, 404);
      EXPECT_NE(result.content_type.find("application/json"), std::string::npos)
          << result.content_type;
      EXPECT_EQ(result.body.find("spa-index"), std::string::npos) << result.body;
      const auto body = nlohmann::json::parse(result.body, nullptr, /*allow_exceptions=*/false);
      ASSERT_TRUE(body.is_object()) << result.body;
      EXPECT_EQ(body.value("code", ""), "NOT_FOUND");
      EXPECT_EQ(result.cache_control, "no-store");
    }

    TEST(RestStaticAssets, ClassifiesApiPathsCaseInsensitively) {
      // Drogon's exact routing lowercases the lookup path, so /API/... can
      // reach a gateway route (or miss one); the fallback and the cache advice
      // must classify it the same way the router did.
      const auto unknown = fetch("/API/v1/does-not-exist");
      ASSERT_TRUE(unknown.ok());
      EXPECT_EQ(unknown.status, 404);
      EXPECT_NE(unknown.content_type.find("application/json"), std::string::npos)
          << unknown.content_type;
      EXPECT_EQ(unknown.cache_control, "no-store");

      const auto known = post(base_url(), "/API/v1/sample/import", R"({"samples":[]})");
      ASSERT_TRUE(known.ok());
      EXPECT_EQ(known.status, 200);
      EXPECT_EQ(known.cache_control, "no-store");
    }

    TEST(RestStaticAssets, KnownApiResponsesAreNoStore) {
      const std::string body = R"({"samples":[{"external_id":"S-1"}]})";
      const auto result = post(base_url(), "/api/v1/sample/import", body);
      ASSERT_TRUE(result.ok());
      EXPECT_EQ(result.status, 200);
      EXPECT_EQ(result.cache_control, "no-store");
      EXPECT_EQ(nlohmann::json::parse(result.body).value("received", std::size_t{0}), body.size());
    }

    TEST(RestStaticAssets, HealthAndMetricsAreNotShadowedByTheSpaFallback) {
      const auto health = fetch("/healthz");
      ASSERT_TRUE(health.ok());
      EXPECT_EQ(health.status, 200);
      EXPECT_EQ(health.body, R"({"status":"ok"})");

      const auto metrics = fetch("/metrics");
      ASSERT_TRUE(metrics.ok());
      EXPECT_EQ(metrics.status, 200);
      EXPECT_NE(metrics.body.find("fmgr_test_metric"), std::string::npos) << metrics.body;
    }

    // 403 is Drogon's own static-router refusal and 404 is the SPA handler's;
    // either is a refusal. What must never happen is a 200, the canary's bytes
    // or the SPA shell pretending the path is a client-side route.
    void expect_refused(const HttpResult& result, const std::string& path) {
      EXPECT_TRUE(result.status == 403 || result.status == 404) << path << ": " << result.status;
      EXPECT_EQ(result.body.find(static_assets::kSecret), std::string::npos) << path;
      EXPECT_EQ(result.body.find("spa-index"), std::string::npos) << path;
    }

    TEST(RestStaticAssets, RefusesDotDotTraversal) {
      const auto result = fetch("/../secret.txt");
      ASSERT_TRUE(result.ok());
      expect_refused(result, "/../secret.txt");
    }

    TEST(RestStaticAssets, RefusesPercentEncodedTraversal) {
      for (const char* path :
           {"/%2e%2e/secret.txt", "/%2e%2e%2fsecret.txt", "/..%2fsecret.txt",
            "/assets/%2e%2e/%2e%2e/secret.txt", "/%2E%2E/secret.txt", "/..%5csecret.txt"}) {
        const auto result = fetch(path);
        ASSERT_TRUE(result.ok()) << path;
        expect_refused(result, path);
      }
    }

    TEST(RestStaticAssets, RefusesASymlinkThatEscapesTheWebRoot) {
      const auto result = fetch("/escape.txt");
      ASSERT_TRUE(result.ok());
      expect_refused(result, "/escape.txt");
    }

    TEST(RestStaticAssets, SendsBrowserSecurityHeadersOnSpaResponses) {
      const auto result = fetch("/labs/anything");
      ASSERT_TRUE(result.ok());
      EXPECT_EQ(result.csp, kExpectedCsp);
      EXPECT_EQ(result.nosniff, "nosniff");
      EXPECT_EQ(result.referrer_policy, "no-referrer");
      // The test listener is plaintext, so HSTS would be a lie.
      EXPECT_TRUE(result.hsts.empty());
    }

    TEST(RestStaticAssets, SendsBrowserSecurityHeadersOnApiResponses) {
      const auto result = fetch("/api/v1/does-not-exist");
      ASSERT_TRUE(result.ok());
      EXPECT_EQ(result.csp, kExpectedCsp);
      EXPECT_EQ(result.nosniff, "nosniff");
      EXPECT_EQ(result.referrer_policy, "no-referrer");
    }

    TEST(RestStaticAssets, AcceptsATwoMebibyteImportBody) {
      // Above Drogon's 1 MiB default: the raised limit is what makes a real CSV
      // import reach the gateway instead of failing with 413.
      const std::string body(static_cast<std::size_t>(2) * 1024 * 1024, 'x');
      const auto result = post(base_url(), "/api/v1/sample/import", body);
      ASSERT_TRUE(result.ok()) << "transport: " << static_cast<int>(result.transport);
      EXPECT_EQ(result.status, 200);
      EXPECT_EQ(nlohmann::json::parse(result.body).value("received", std::size_t{0}), body.size());
    }

  } // namespace
} // namespace fmgr::test
