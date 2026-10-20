// SPDX-License-Identifier: AGPL-3.0-or-later

// Pins "FMGR_WEB_ROOT unset behaves exactly as before" (TODO.md G0.3, first
// acceptance criterion). StaticAssets::install() must register neither the SPA
// fallback nor the security-header advice, so unknown paths keep Drogon's
// default 404 and no browser headers appear on API responses.

#include "rest/StaticAssets.h"
#include "rest_static_assets_harness.h"

#include <drogon/drogon.h>

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>

namespace fmgr::test {
  namespace {

    using static_assets::find_free_port;
    using static_assets::get;
    using static_assets::wait_until_running;

    class RestStaticAssetsDisabledEnv : public ::testing::Environment {
    public:
      static RestStaticAssetsDisabledEnv* instance;

      void SetUp() override {
        instance = this;
        ::unsetenv("FMGR_WEB_ROOT");
        ::unsetenv("FMGR_REST_MAX_BODY_BYTES");

        const auto config = rest::StaticAssetsConfig::from_env(/*tls=*/false);
        if (config.serves_spa()) {
          ADD_FAILURE() << "FMGR_WEB_ROOT leaked into the disabled configuration";
          return;
        }
        // A route on disk the SPA would serve if it were enabled: it must stay
        // invisible, not even as a fallback.
        web_ = static_assets::WebRoot::create("disabled");

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

    private:
      static_assets::WebRoot web_;
      std::uint16_t port_{0};
      std::thread app_thread_;
      bool started_{false};
    };

    RestStaticAssetsDisabledEnv* RestStaticAssetsDisabledEnv::instance = nullptr;

    const ::testing::Environment* const kEnv =
        ::testing::AddGlobalTestEnvironment(new RestStaticAssetsDisabledEnv);

    [[nodiscard]] std::string base_url() {
      return RestStaticAssetsDisabledEnv::instance->base_url();
    }

    TEST(RestStaticAssetsDisabled, DoesNotServeFilesFromTheWebRoot) {
      for (const char* path : {"/", "/index.html", "/assets/app-abc123.js"}) {
        const auto result = get(base_url(), path);
        ASSERT_TRUE(result.ok()) << path;
        EXPECT_EQ(result.status, 404) << path;
        EXPECT_EQ(result.body.find("spa-index"), std::string::npos) << path;
      }
    }

    TEST(RestStaticAssetsDisabled, DoesNotAddTheSpaSecurityHeaders) {
      const auto result = get(base_url(), "/");
      ASSERT_TRUE(result.ok());
      EXPECT_EQ(result.status, 404);
      EXPECT_TRUE(result.csp.empty());
      EXPECT_TRUE(result.nosniff.empty());
      EXPECT_TRUE(result.referrer_policy.empty());
      EXPECT_TRUE(result.hsts.empty());
    }

    TEST(RestStaticAssetsDisabled, KeepsTheFramework404ForUnknownApiPaths) {
      // With the SPA disabled nothing claims /api/*; Drogon's default 404 page
      // is what callers got before G0.3. (With the SPA enabled the fallback
      // turns this into a JSON 404 — see rest_static_assets_integration_test.)
      const auto result = get(base_url(), "/api/v1/does-not-exist");
      ASSERT_TRUE(result.ok());
      EXPECT_EQ(result.status, 404);
      EXPECT_EQ(result.body.find("spa-index"), std::string::npos);
      EXPECT_TRUE(result.cache_control.empty());
    }

  } // namespace
} // namespace fmgr::test
