// SPDX-License-Identifier: AGPL-3.0-or-later

// Unit tests for the browser-session cookie and CSRF helpers (TODO.md G0.1).
// These are the pure decision points behind RestGateway.cc's browser routes:
// the cookie attributes, the CSRF double-submit comparison, the Origin check
// and the environment rules. The HTTP-level behaviour they drive is covered end
// to end in tests/integration/rest_gateway_integration_test.cpp.

#include "rest/BrowserSession.h"

#include <drogon/HttpRequest.h>

#include <gtest/gtest.h>

#include <cstdlib>
#include <optional>
#include <string>

namespace fmgr::rest {
  namespace {

    // Every test starts from the same explicit environment, so a stray export in
    // the developer's shell cannot change the outcome.
    class BrowserSessionTest : public ::testing::Test {
    protected:
      void SetUp() override {
        clear_env("FMGR_DEV_INSECURE_COOKIES");
        clear_env("FMGR_WEB_ORIGIN");
        clear_env("FMGR_ENV");
      }

      void TearDown() override {
        clear_env("FMGR_DEV_INSECURE_COOKIES");
        clear_env("FMGR_WEB_ORIGIN");
        clear_env("FMGR_ENV");
      }

      static void set_env(const char* name, const char* value) {
        ::setenv(name, value, 1);
      }

      static void clear_env(const char* name) {
        ::unsetenv(name);
      }

      // A request as the gateway sees it, with only the fields under test set.
      [[nodiscard]] static BrowserRequest request(std::string method = "POST") {
        BrowserRequest req;
        req.method = std::move(method);
        req.host = "fmgr.example.test";
        return req;
      }
    };

    // ---- Cookie attributes ----

    TEST_F(BrowserSessionTest, SessionCookieIsHttpOnlySecureSameSiteStrictAndScopedToApi) {
      const BrowserSessionConfig config;
      const auto cookie = session_cookie("abc123", config);

      EXPECT_EQ(cookie.key(), "fmgr_session");
      EXPECT_EQ(cookie.value(), "abc123");
      EXPECT_EQ(cookie.path(), "/api");
      EXPECT_TRUE(cookie.isHttpOnly());
      EXPECT_TRUE(cookie.isSecure());
      EXPECT_EQ(cookie.sameSite(), drogon::Cookie::SameSite::kStrict);
      // cookieString() is the header line, CRLF included.
      EXPECT_EQ(
          cookie.cookieString(),
          "Set-Cookie: fmgr_session=abc123; Path=/api; SameSite=Strict; Secure; HttpOnly\r\n");
    }

    // Server-side idle and absolute expiry are the real limits, so the cookie is
    // a session cookie: a Max-Age would let a stolen value outlive revocation.
    TEST_F(BrowserSessionTest, SessionCookieCarriesNoMaxAge) {
      const BrowserSessionConfig config;
      EXPECT_FALSE(session_cookie("abc123", config).maxAge().has_value());
    }

    TEST_F(BrowserSessionTest, CsrfCookieIsScriptReadableSecureSameSiteStrictAndScopedToRoot) {
      const BrowserSessionConfig config;
      const auto cookie = csrf_cookie("tok-value", config);

      EXPECT_EQ(cookie.key(), "fmgr_csrf");
      EXPECT_EQ(cookie.value(), "tok-value");
      EXPECT_EQ(cookie.path(), "/");
      EXPECT_FALSE(cookie.isHttpOnly());
      EXPECT_TRUE(cookie.isSecure());
      EXPECT_EQ(cookie.sameSite(), drogon::Cookie::SameSite::kStrict);
    }

    // FMGR_DEV_INSECURE_COOKIES=1 exists because Safari refuses to store a
    // Secure cookie from http://127.0.0.1. It drops Secure and nothing else.
    TEST_F(BrowserSessionTest, InsecureCookiesDropSecureButKeepTheOtherAttributes) {
      BrowserSessionConfig config;
      config.secure_cookies = false;

      const auto session = session_cookie("abc123", config);
      EXPECT_FALSE(session.isSecure());
      EXPECT_TRUE(session.isHttpOnly());
      EXPECT_EQ(session.sameSite(), drogon::Cookie::SameSite::kStrict);
      EXPECT_EQ(session.path(), "/api");
      EXPECT_EQ(session.cookieString(),
                "Set-Cookie: fmgr_session=abc123; Path=/api; SameSite=Strict; HttpOnly\r\n");

      const auto csrf = csrf_cookie("tok-value", config);
      EXPECT_FALSE(csrf.isSecure());
      EXPECT_FALSE(csrf.isHttpOnly());
      EXPECT_EQ(csrf.path(), "/");
    }

    TEST_F(BrowserSessionTest, ExpiredCookiesClearValueAndCarryMaxAgeZero) {
      const BrowserSessionConfig config;

      const auto session = expired_session_cookie(config);
      EXPECT_EQ(session.key(), "fmgr_session");
      EXPECT_TRUE(session.value().empty());
      EXPECT_EQ(session.maxAge(), std::optional<int>{0});
      EXPECT_EQ(session.expiresDate().microSecondsSinceEpoch(), 0);
      EXPECT_EQ(session.path(), "/api");
      EXPECT_TRUE(session.isHttpOnly());
      EXPECT_TRUE(session.isSecure());

      const auto csrf = expired_csrf_cookie(config);
      EXPECT_EQ(csrf.key(), "fmgr_csrf");
      EXPECT_TRUE(csrf.value().empty());
      EXPECT_EQ(csrf.maxAge(), std::optional<int>{0});
      EXPECT_EQ(csrf.path(), "/");
      EXPECT_FALSE(csrf.isHttpOnly());
    }

    // ---- CSRF token ----

    TEST_F(BrowserSessionTest, GenerateCsrfTokenIs32BytesOfBase64UrlWithoutPadding) {
      const std::string token = generate_csrf_token();

      // 32 bytes base64 -> ceil(32/3)*4 = 44 chars, minus one '=' pad.
      EXPECT_EQ(token.size(), 43U);
      EXPECT_EQ(token.find('='), std::string::npos);
      for (const char c : token) {
        const bool url_safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                              (c >= '0' && c <= '9') || c == '-' || c == '_';
        EXPECT_TRUE(url_safe) << "unexpected character in CSRF token: " << c;
      }
    }

    TEST_F(BrowserSessionTest, GenerateCsrfTokenIsNotRepeated) {
      EXPECT_NE(generate_csrf_token(), generate_csrf_token());
    }

    // ---- Bearer resolution: Authorization header wins over the cookie ----

    TEST_F(BrowserSessionTest, AuthorizationHeaderWinsOverTheSessionCookie) {
      auto req = request();
      req.authorization = "Bearer script-token";
      req.session_cookie = "cookie-token";

      EXPECT_EQ(authorization_metadata(req), "Bearer script-token");
    }

    TEST_F(BrowserSessionTest, SessionCookieIsForwardedAsBearerMetadataWithoutAHeader) {
      auto req = request();
      req.session_cookie = "cookie-token";

      EXPECT_EQ(authorization_metadata(req), "Bearer cookie-token");
    }

    TEST_F(BrowserSessionTest, NoCredentialProducesNoAuthorizationMetadata) {
      EXPECT_TRUE(authorization_metadata(request()).empty());
    }

    // ---- CSRF / Origin gate ----

    TEST_F(BrowserSessionTest, CookiePostWithMatchingCsrfHeaderIsAllowed) {
      const BrowserSessionConfig config;
      auto req = request();
      req.session_cookie = "s";
      req.csrf_cookie = "c";
      req.csrf_header = "c";

      EXPECT_FALSE(csrf_denial(req, config).has_value());
    }

    TEST_F(BrowserSessionTest, CookiePostWithoutCsrfHeaderIsDenied) {
      const BrowserSessionConfig config;
      auto req = request();
      req.session_cookie = "s";
      req.csrf_cookie = "c";

      ASSERT_TRUE(csrf_denial(req, config).has_value());
    }

    TEST_F(BrowserSessionTest, CookiePostWithWrongCsrfHeaderIsDenied) {
      const BrowserSessionConfig config;
      auto req = request();
      req.session_cookie = "s";
      req.csrf_cookie = "c";
      req.csrf_header = "not-c";

      ASSERT_TRUE(csrf_denial(req, config).has_value());
    }

    // A cookie-authenticated request with no CSRF cookie at all must fail closed
    // rather than comparing two empty strings.
    TEST_F(BrowserSessionTest, CookiePostWithNoCsrfCookieIsDenied) {
      const BrowserSessionConfig config;
      auto req = request();
      req.session_cookie = "s";

      ASSERT_TRUE(csrf_denial(req, config).has_value());
    }

    // GET is a safe method: the SSE feeds are cookie-authenticated but have
    // nothing to forge.
    TEST_F(BrowserSessionTest, SafeMethodWithSessionCookieSkipsTheCsrfCheck) {
      const BrowserSessionConfig config;
      auto req = request("GET");
      req.session_cookie = "s";
      req.csrf_cookie = "c";

      EXPECT_FALSE(csrf_denial(req, config).has_value());
    }

    // Bearer calls carry no ambient credential, so they skip both halves of the
    // gate even when a stale cookie rides along.
    TEST_F(BrowserSessionTest, BearerCallSkipsTheCsrfAndOriginChecks) {
      const BrowserSessionConfig config;
      auto req = request();
      req.authorization = "Bearer script-token";
      req.session_cookie = "s";
      req.origin = "https://evil.example";

      EXPECT_FALSE(csrf_denial(req, config).has_value());
    }

    TEST_F(BrowserSessionTest, OriginMatchingTheRequestHostIsAllowed) {
      const BrowserSessionConfig config;
      auto req = request();
      req.session_cookie = "s";
      req.csrf_cookie = "c";
      req.csrf_header = "c";
      req.origin = "https://fmgr.example.test";

      EXPECT_FALSE(csrf_denial(req, config).has_value());
    }

    TEST_F(BrowserSessionTest, OriginWithADevPortMatchingTheHostIsAllowed) {
      const BrowserSessionConfig config;
      auto req = request();
      req.host = "127.0.0.1:5173";
      req.session_cookie = "s";
      req.csrf_cookie = "c";
      req.csrf_header = "c";
      req.origin = "http://127.0.0.1:5173";

      EXPECT_FALSE(csrf_denial(req, config).has_value());
    }

    TEST_F(BrowserSessionTest, ForeignOriginIsDenied) {
      const BrowserSessionConfig config;
      auto req = request();
      req.session_cookie = "s";
      req.csrf_cookie = "c";
      req.csrf_header = "c";
      req.origin = "https://evil.example";

      ASSERT_TRUE(csrf_denial(req, config).has_value());
    }

    // A cross-site HTML form POST carries no cookies at all (the session cookie
    // is SameSite=Strict), so the Origin check has to stand on its own or login
    // CSRF would be open.
    TEST_F(BrowserSessionTest, ForeignOriginIsDeniedEvenWithoutASessionCookie) {
      const BrowserSessionConfig config;
      auto req = request();
      req.origin = "https://evil.example";

      ASSERT_TRUE(csrf_denial(req, config).has_value());
    }

    TEST_F(BrowserSessionTest, ConfiguredWebOriginIsAllowed) {
      BrowserSessionConfig config;
      config.web_origin = "https://spa.example.test";
      auto req = request();
      req.session_cookie = "s";
      req.csrf_cookie = "c";
      req.csrf_header = "c";
      req.origin = "https://spa.example.test";

      EXPECT_FALSE(csrf_denial(req, config).has_value());
    }

    TEST_F(BrowserSessionTest, OriginThatIsNeitherHostNorConfiguredWebOriginIsDenied) {
      BrowserSessionConfig config;
      config.web_origin = "https://spa.example.test";
      auto req = request();
      req.session_cookie = "s";
      req.csrf_cookie = "c";
      req.csrf_header = "c";
      req.origin = "https://evil.example";

      ASSERT_TRUE(csrf_denial(req, config).has_value());
    }

    // `Origin: null` (sandboxed iframe, file://) must not be read as "no origin".
    TEST_F(BrowserSessionTest, NullOriginIsDenied) {
      const BrowserSessionConfig config;
      auto req = request();
      req.origin = "null";

      ASSERT_TRUE(csrf_denial(req, config).has_value());
    }

    TEST_F(BrowserSessionTest, MissingOriginIsAllowedForANonBrowserClient) {
      const BrowserSessionConfig config;
      auto req = request();
      req.session_cookie = "s";
      req.csrf_cookie = "c";
      req.csrf_header = "c";

      EXPECT_FALSE(csrf_denial(req, config).has_value());
    }

    // ---- Environment ----

    TEST_F(BrowserSessionTest, SecureCookiesAreTheDefault) {
      EXPECT_TRUE(BrowserSessionConfig::from_env().secure_cookies);
    }

    TEST_F(BrowserSessionTest, InsecureCookieFlagIsReadFromTheEnvironment) {
      set_env("FMGR_DEV_INSECURE_COOKIES", "1");
      EXPECT_FALSE(BrowserSessionConfig::from_env().secure_cookies);
    }

    TEST_F(BrowserSessionTest, InsecureCookieFlagMustBeExactlyOne) {
      set_env("FMGR_DEV_INSECURE_COOKIES", "true");
      EXPECT_TRUE(BrowserSessionConfig::from_env().secure_cookies);
    }

    TEST_F(BrowserSessionTest, WebOriginIsReadFromTheEnvironment) {
      set_env("FMGR_WEB_ORIGIN", "https://spa.example.test");
      EXPECT_EQ(BrowserSessionConfig::from_env().web_origin, "https://spa.example.test");
    }

    // A trailing slash is what a browser URL bar produces; normalize it away so
    // the operator cannot silently disable the check with it.
    TEST_F(BrowserSessionTest, WebOriginIsNormalized) {
      set_env("FMGR_WEB_ORIGIN", "HTTPS://SPA.Example.Test/");
      EXPECT_EQ(BrowserSessionConfig::from_env().web_origin, "https://spa.example.test");
    }

    TEST_F(BrowserSessionTest, StartupIsAllowedWithoutTheInsecureCookieFlag) {
      set_env("FMGR_ENV", "production");
      EXPECT_TRUE(validate_browser_session_env().empty());
    }

    TEST_F(BrowserSessionTest, StartupIsAllowedWithInsecureCookiesOutsideProduction) {
      set_env("FMGR_DEV_INSECURE_COOKIES", "1");
      set_env("FMGR_ENV", "development");
      EXPECT_TRUE(validate_browser_session_env().empty());
    }

    TEST_F(BrowserSessionTest, StartupRefusesInsecureCookiesInProduction) {
      set_env("FMGR_DEV_INSECURE_COOKIES", "1");
      set_env("FMGR_ENV", "production");

      const std::string reason = validate_browser_session_env();
      ASSERT_FALSE(reason.empty());
      EXPECT_NE(reason.find("FMGR_DEV_INSECURE_COOKIES"), std::string::npos);
      EXPECT_NE(reason.find("production"), std::string::npos);
    }

    // ---- Adapter over a real drogon request ----

    TEST_F(BrowserSessionTest, BrowserRequestFromReadsMethodCookiesAndHeaders) {
      auto req = drogon::HttpRequest::newHttpRequest();
      req->setMethod(drogon::Post);
      req->addHeader("Authorization", "Bearer script-token");
      req->addHeader("Origin", "https://fmgr.example.test");
      req->addHeader("Host", "fmgr.example.test");
      req->addHeader("X-CSRF-Token", "csrf-value");
      req->addCookie("fmgr_session", "session-value");
      req->addCookie("fmgr_csrf", "csrf-value");

      const auto extracted = browser_request_from(*req);

      EXPECT_EQ(extracted.method, "POST");
      EXPECT_EQ(extracted.authorization, "Bearer script-token");
      EXPECT_EQ(extracted.session_cookie, "session-value");
      EXPECT_EQ(extracted.csrf_cookie, "csrf-value");
      EXPECT_EQ(extracted.csrf_header, "csrf-value");
      EXPECT_EQ(extracted.origin, "https://fmgr.example.test");
      EXPECT_EQ(extracted.host, "fmgr.example.test");
      EXPECT_EQ(authorization_metadata(extracted), "Bearer script-token");
      EXPECT_FALSE(csrf_denial(extracted, BrowserSessionConfig{}).has_value());
    }

    TEST_F(BrowserSessionTest, BrowserRequestFromTreatsAGetAsSafe) {
      auto req = drogon::HttpRequest::newHttpRequest();
      req->setMethod(drogon::Get);
      req->addCookie("fmgr_session", "session-value");

      const auto extracted = browser_request_from(*req);

      EXPECT_EQ(extracted.method, "GET");
      EXPECT_TRUE(extracted.authorization.empty());
      EXPECT_FALSE(csrf_denial(extracted, BrowserSessionConfig{}).has_value());
    }

  } // namespace
} // namespace fmgr::rest
