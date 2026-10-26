// SPDX-License-Identifier: AGPL-3.0-or-later

// Unit tests for the SPA static-asset configuration and path resolution
// (TODO.md G0.3). The HTTP-level behaviour these decisions drive — headers,
// cache policy, deep-link fallback and the 413 body limit — is covered end to
// end in tests/integration/rest_static_assets*_integration_test.cpp.

#include "rest/StaticAssets.h"

#include <gtest/gtest.h>

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace fmgr::rest {
  namespace {

    class RestStaticAssetsConfig : public ::testing::Test {
    protected:
      void SetUp() override {
        ::unsetenv("FMGR_WEB_ROOT");
        ::unsetenv("FMGR_REST_MAX_BODY_BYTES");
      }

      void TearDown() override {
        ::unsetenv("FMGR_WEB_ROOT");
        ::unsetenv("FMGR_REST_MAX_BODY_BYTES");
      }
    };

    TEST_F(RestStaticAssetsConfig, UnsetWebRootDisablesStaticServing) {
      const auto config = StaticAssetsConfig::from_env(/*tls=*/false);
      EXPECT_FALSE(config.serves_spa());
      EXPECT_TRUE(config.web_root.empty());
    }

    TEST_F(RestStaticAssetsConfig, EmptyWebRootDisablesStaticServing) {
      ::setenv("FMGR_WEB_ROOT", "", 1);
      const auto config = StaticAssetsConfig::from_env(/*tls=*/false);
      EXPECT_FALSE(config.serves_spa());
    }

    TEST_F(RestStaticAssetsConfig, WebRootComesFromTheEnvironment) {
      ::setenv("FMGR_WEB_ROOT", "/srv/freezerd/dist", 1);
      const auto config = StaticAssetsConfig::from_env(/*tls=*/false);
      EXPECT_TRUE(config.serves_spa());
      EXPECT_EQ(config.web_root, std::filesystem::path("/srv/freezerd/dist"));
    }

    TEST_F(RestStaticAssetsConfig, TlsFlagComesFromTheListener) {
      EXPECT_FALSE(StaticAssetsConfig::from_env(/*tls=*/false).tls);
      EXPECT_TRUE(StaticAssetsConfig::from_env(/*tls=*/true).tls);
    }

    // 10 MiB is FreezerServerOptions::max_receive_message_bytes (C-10): an import
    // that fits the gRPC inbound cap must not die at Drogon's 1 MiB default.
    TEST_F(RestStaticAssetsConfig, BodyLimitDefaultsToTheGrpcInboundCap) {
      const auto config = StaticAssetsConfig::from_env(/*tls=*/false);
      EXPECT_EQ(config.max_body_bytes, std::size_t{10} * 1024 * 1024);
    }

    TEST_F(RestStaticAssetsConfig, BodyLimitComesFromTheEnvironment) {
      ::setenv("FMGR_REST_MAX_BODY_BYTES", "2097152", 1);
      const auto config = StaticAssetsConfig::from_env(/*tls=*/false);
      EXPECT_EQ(config.max_body_bytes, std::size_t{2} * 1024 * 1024);
    }

    TEST_F(RestStaticAssetsConfig, EmptyBodyLimitFallsBackToTheDefault) {
      ::setenv("FMGR_REST_MAX_BODY_BYTES", "", 1);
      const auto config = StaticAssetsConfig::from_env(/*tls=*/false);
      EXPECT_EQ(config.max_body_bytes, std::size_t{10} * 1024 * 1024);
    }

    TEST_F(RestStaticAssetsConfig, MalformedBodyLimitIsRejected) {
      for (const char* value :
           {"abc", "0", "-1", "1024x", "1.5", " 1024", "99999999999999999999999"}) {
        ::setenv("FMGR_REST_MAX_BODY_BYTES", value, 1);
        EXPECT_THROW(StaticAssetsConfig::from_env(/*tls=*/false), std::invalid_argument)
            << "accepted FMGR_REST_MAX_BODY_BYTES=" << value;
      }
    }

    class RestStaticAssetsResolve : public ::testing::Test {
    protected:
      void SetUp() override {
        base_ = std::filesystem::temp_directory_path() /
                ("fmgr-rest-static-assets-unit-" + std::to_string(::getpid()));
        std::error_code ec;
        std::filesystem::remove_all(base_, ec);
        root_ = base_ / "web";
        std::filesystem::create_directories(root_ / "assets");
        write(root_ / "index.html", "<html>index</html>");
        write(root_ / "assets" / "app-abc123.js", "console.log('asset');");
        write(base_ / "secret.txt", "TOP-SECRET");
        std::filesystem::create_symlink(base_ / "secret.txt", root_ / "escape.txt");
      }

      void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(base_, ec);
      }

      static void write(const std::filesystem::path& path, std::string_view contents) {
        std::ofstream out(path, std::ios::binary);
        out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
      }

      [[nodiscard]] static std::filesystem::path canonical(const std::filesystem::path& path) {
        return std::filesystem::weakly_canonical(path);
      }

      std::filesystem::path base_;
      std::filesystem::path root_;
    };

    TEST_F(RestStaticAssetsResolve, ServesARegularFileInsideTheWebRoot) {
      const auto resolved = StaticAssets::resolve_path(root_, "/assets/app-abc123.js");
      ASSERT_TRUE(resolved.has_value());
      EXPECT_EQ(*resolved, canonical(root_ / "assets" / "app-abc123.js"));
    }

    TEST_F(RestStaticAssetsResolve, FallsBackToIndexHtmlForTheRoot) {
      const auto resolved = StaticAssets::resolve_path(root_, "/");
      ASSERT_TRUE(resolved.has_value());
      EXPECT_EQ(*resolved, canonical(root_ / "index.html"));
    }

    TEST_F(RestStaticAssetsResolve, FallsBackToIndexHtmlForAClientSideRoute) {
      const auto resolved = StaticAssets::resolve_path(root_, "/labs/lab-1/samples/sample-9");
      ASSERT_TRUE(resolved.has_value());
      EXPECT_EQ(*resolved, canonical(root_ / "index.html"));
    }

    TEST_F(RestStaticAssetsResolve, FallsBackToIndexHtmlForAMissingAsset) {
      // Vite deep links never look like assets, but a stale hashed name is a
      // client-side navigation as far as the server is concerned; the advice
      // keeps it out of the immutable cache because the response is HTML.
      const auto resolved = StaticAssets::resolve_path(root_, "/assets/gone-000000.js");
      ASSERT_TRUE(resolved.has_value());
      EXPECT_EQ(*resolved, canonical(root_ / "index.html"));
    }

    TEST_F(RestStaticAssetsResolve, RejectsDotDotSegments) {
      for (const char* path : {"/../secret.txt", "/assets/../../secret.txt", "/..", "/a/../b"}) {
        EXPECT_FALSE(StaticAssets::resolve_path(root_, path).has_value()) << path;
      }
    }

    TEST_F(RestStaticAssetsResolve, RejectsDotSegments) {
      for (const char* path : {"/./index.html", "/assets/./app-abc123.js", "/."}) {
        EXPECT_FALSE(StaticAssets::resolve_path(root_, path).has_value()) << path;
      }
    }

    TEST_F(RestStaticAssetsResolve, RejectsBackslashSegments) {
      for (const char* path : {"/assets\\..\\secret.txt", "/..\\secret.txt", "/a\\b"}) {
        EXPECT_FALSE(StaticAssets::resolve_path(root_, path).has_value()) << path;
      }
    }

    TEST_F(RestStaticAssetsResolve, RejectsANulByteInThePath) {
      std::string path("/index.html");
      path.push_back('\0');
      path += "../../secret.txt";
      EXPECT_FALSE(StaticAssets::resolve_path(root_, path).has_value());
    }

    TEST_F(RestStaticAssetsResolve, RejectsASymlinkThatEscapesTheWebRoot) {
      // `escape.txt` is inside the root but points at base_/secret.txt. The
      // canonical containment check must refuse it instead of serving the file.
      EXPECT_FALSE(StaticAssets::resolve_path(root_, "/escape.txt").has_value());
    }

  } // namespace
} // namespace fmgr::rest
