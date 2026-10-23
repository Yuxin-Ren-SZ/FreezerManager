// SPDX-License-Identifier: AGPL-3.0-or-later

// End-to-end tests for the two SPA static-asset behaviours that need their own
// process-wide Drogon configuration (TODO.md G0.3):
//
//   * HSTS is sent only when the REST listener terminates TLS;
//   * FMGR_REST_MAX_BODY_BYTES bounds the HTTP request body, so a 2 MiB import
//     that the default (10 MiB) limit accepts is rejected with 413 when the
//     operator lowers it.
//
// The HTTPS listener itself is not needed to test the header decision: `install`
// is told whether the listener is TLS (`from_env(/*tls=*/true)`), exactly as
// main.cc does.

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
    using static_assets::post;
    using static_assets::wait_until_running;
    using static_assets::WebRoot;

    constexpr std::size_t kLoweredBodyLimit = 1024;

    class RestStaticAssetsTlsEnv : public ::testing::Environment {
    public:
      static RestStaticAssetsTlsEnv* instance;

      void SetUp() override {
        instance = this;
        working_dir_.create("tls");
        ::unsetenv("FMGR_WEB_ROOT");
        ::unsetenv("FMGR_REST_MAX_BODY_BYTES");
        web_ = WebRoot::create("tls");
        ::setenv("FMGR_WEB_ROOT", web_.root.string().c_str(), 1);
        ::setenv("FMGR_REST_MAX_BODY_BYTES", "1024", 1);

        const auto config = rest::StaticAssetsConfig::from_env(/*tls=*/true);
        if (!config.serves_spa()) {
          ADD_FAILURE() << "FMGR_WEB_ROOT did not enable static serving";
          return;
        }
        if (config.max_body_bytes != kLoweredBodyLimit) {
          ADD_FAILURE() << "FMGR_REST_MAX_BODY_BYTES was not honoured";
          return;
        }

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
        working_dir_.restore_and_remove();
      }

      [[nodiscard]] std::uint16_t port() const {
        return port_;
      }

    private:
      static_assets::WorkingDirectory working_dir_;
      WebRoot web_;
      std::uint16_t port_{0};
      std::thread app_thread_;
      bool started_{false};
    };

    RestStaticAssetsTlsEnv* RestStaticAssetsTlsEnv::instance = nullptr;

    const ::testing::Environment* const kEnv =
        ::testing::AddGlobalTestEnvironment(new RestStaticAssetsTlsEnv);

    [[nodiscard]] std::uint16_t port() {
      return RestStaticAssetsTlsEnv::instance->port();
    }

    TEST(RestStaticAssetsTls, SendsHstsWhenTheRestListenerTerminatesTls) {
      const auto result = get(port(), "/");
      ASSERT_TRUE(result.ok());
      EXPECT_EQ(result.status, 200);
      EXPECT_EQ(result.hsts, "max-age=31536000");
      EXPECT_NE(result.csp.find("default-src 'self'"), std::string::npos) << result.csp;
      EXPECT_EQ(result.nosniff, "nosniff");
    }

    TEST(RestStaticAssetsTls, AcceptsAnImportBodyWithinTheConfiguredLimit) {
      const std::string body(static_cast<std::size_t>(512), 'x');
      const auto result = post(port(), "/api/v1/sample/import", body);
      ASSERT_TRUE(result.ok());
      EXPECT_EQ(result.status, 200);
      EXPECT_EQ(nlohmann::json::parse(result.body).value("received", std::size_t{0}), body.size());
    }

    TEST(RestStaticAssetsTls, RejectsATwoMebibyteImportBodyWhenTheLimitIsLowered) {
      const std::string body(static_cast<std::size_t>(2) * 1024 * 1024, 'x');
      const auto result = post(port(), "/api/v1/sample/import", body);
      EXPECT_EQ(result.status, 413);
    }

  } // namespace
} // namespace fmgr::test
