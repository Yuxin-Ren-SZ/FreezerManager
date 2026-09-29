// SPDX-License-Identifier: AGPL-3.0-or-later

// Browser session cookies and the CSRF / Origin gate (TODO.md G0.1).
//
// A browser SPA cannot hold a bearer token: anything JavaScript can read can be
// read by an XSS payload, and a token in a URL ends up in proxy and access logs.
// So the gateway hands the browser an `HttpOnly` session cookie and keeps the
// token out of JavaScript and out of response bodies.
//
// The cookie is an ambient credential — the browser attaches it to any request
// to this origin, including one triggered by another site. Two defences cover
// that, and this module owns both:
//
//   * `SameSite=Strict`, set on both cookies, keeps them off cross-site requests;
//   * a double-submit token (`X-CSRF-Token` must equal the JS-readable
//     `fmgr_csrf` cookie) for cookie-authenticated mutations, plus an `Origin`
//     check for every unauthenticated mutation.
//
// The `Origin` check deliberately does not require a session cookie: a
// cross-site form POST arrives with no cookies at all, so if the check waited for
// one it would never fire where it matters most (login CSRF).
//
// Everything here is a pure function of its arguments except `from_env()` and
// `validate_browser_session_env()`, which read the process environment. That
// keeps the security-relevant decisions unit-testable without an HTTP server;
// `browser_request_from()` is the thin adapter that pulls the five fields the
// gate needs out of a drogon request.
#ifndef FMGR_REST_BROWSERSESSION_H
#define FMGR_REST_BROWSERSESSION_H

#include <drogon/Cookie.h>
#include <drogon/HttpRequest.h>

#include <optional>
#include <string>
#include <string_view>

namespace fmgr::rest {

  // The session cookie's value is the bearer token from AuthService.Login. It is
  // scoped to /api so it is not sent with static assets (G0.3 serves the SPA).
  inline constexpr std::string_view k_session_cookie_name = "fmgr_session";
  inline constexpr std::string_view k_session_cookie_path = "/api";

  // The CSRF cookie is deliberately readable by JavaScript: the SPA copies it
  // into the X-CSRF-Token header, which is what makes the check a *double*
  // submit. It is scoped to / so the SPA shell can read it too.
  inline constexpr std::string_view k_csrf_cookie_name = "fmgr_csrf";
  inline constexpr std::string_view k_csrf_cookie_path = "/";
  inline constexpr std::string_view k_csrf_header_name = "X-CSRF-Token";

  // The browser routes (G0.1). RestGateway.cc registers these; they are named
  // here because the success-response decorator switches on them.
  inline constexpr std::string_view k_browser_login_path = "/api/v1/auth/browser/login";
  inline constexpr std::string_view k_browser_submit_mfa_path = "/api/v1/auth/browser/submit-mfa";
  inline constexpr std::string_view k_browser_logout_path = "/api/v1/auth/browser/logout";

  // The cookie policy, read once at startup.
  struct BrowserSessionConfig {
    // `Secure` on both cookies. Off only for FMGR_DEV_INSECURE_COOKIES=1, which
    // exists because Safari will not store a Secure cookie from
    // http://127.0.0.1 — and which production refuses at startup.
    bool secure_cookies{true};
    // Normalized `scheme://host[:port]` from FMGR_WEB_ORIGIN: an extra origin the
    // Origin check accepts, for a deployment that serves the SPA somewhere else.
    // Empty when unset.
    std::string web_origin;

    [[nodiscard]] static BrowserSessionConfig from_env();
  };

  // Startup guard. Returns an empty string when the process may start, else the
  // operator-facing reason it must not: `FMGR_DEV_INSECURE_COOKIES=1` together
  // with `FMGR_ENV=production` would silently drop `Secure` in production.
  [[nodiscard]] std::string validate_browser_session_env();

  // 32 random bytes, base64url, unpadded (43 characters). URL- and cookie-safe by
  // construction, which is what lets the SPA echo it in a header unencoded.
  [[nodiscard]] std::string generate_csrf_token();

  // `fmgr_session=<token>; HttpOnly; Secure; SameSite=Strict; Path=/api`, and no
  // `Max-Age`: server-side idle and absolute expiry are the real limits, so the
  // cookie is a session cookie that cannot outlive a server-side revocation.
  [[nodiscard]] drogon::Cookie session_cookie(std::string token,
                                              const BrowserSessionConfig& config);

  // `fmgr_csrf=<32 bytes base64url>; Secure; SameSite=Strict; Path=/`, readable by
  // JavaScript.
  [[nodiscard]] drogon::Cookie csrf_cookie(std::string token, const BrowserSessionConfig& config);

  // Deletion cookies for logout: empty value, `Max-Age=0` and the epoch as
  // `Expires` (some clients still key off the latter).
  [[nodiscard]] drogon::Cookie expired_session_cookie(const BrowserSessionConfig& config);
  [[nodiscard]] drogon::Cookie expired_csrf_cookie(const BrowserSessionConfig& config);

  // The five things the gate looks at, pulled out of an HTTP request.
  struct BrowserRequest {
    std::string method;        // "POST", "GET", … — uppercase, from drogon
    std::string authorization; // Authorization header, empty when absent
    std::string session_cookie;
    std::string csrf_cookie;
    std::string csrf_header;
    std::string origin; // Origin header, empty when absent
    std::string host;   // Host header
  };

  [[nodiscard]] BrowserRequest browser_request_from(const drogon::HttpRequest& req);

  // The gRPC `authorization` metadata to forward: the `Authorization` header wins
  // (scripts, CLI, the Qt client); without one the session cookie is rendered as
  // `Bearer <token>`. Empty when the request carries no credential.
  [[nodiscard]] std::string authorization_metadata(const BrowserRequest& req);

  // The CSRF / Origin gate. `nullopt` means the request may proceed; a value is
  // the reason it must not, which RestGateway.cc turns into
  // `403 {"code":"PERMISSION_DENIED"}` without calling gRPC.
  //
  // Rules, in order:
  //   * safe methods (GET/HEAD/OPTIONS) are never gated — the SSE feeds are
  //     cookie-authenticated but read-only, so there is nothing to forge;
  //   * a request with an `Authorization` header is never gated — a bearer token
  //     is not an ambient credential, so no other site can make the browser send
  //     it;
  //   * a present `Origin` must be the request's own authority or FMGR_WEB_ORIGIN;
  //     `Origin: null` matches neither and is refused;
  //   * a request carrying `fmgr_session` must echo a non-empty `fmgr_csrf`
  //     cookie in `X-CSRF-Token`.
  [[nodiscard]] std::optional<std::string> csrf_denial(const BrowserRequest& req,
                                                       const BrowserSessionConfig& config);

} // namespace fmgr::rest

#endif // FMGR_REST_BROWSERSESSION_H
