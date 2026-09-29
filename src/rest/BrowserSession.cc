// SPDX-License-Identifier: AGPL-3.0-or-later

#include "rest/BrowserSession.h"

#include <sodium.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace fmgr::rest {
  namespace {

    constexpr std::string_view k_base64url_alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

    constexpr std::size_t k_csrf_token_bytes = 32;

    [[nodiscard]] std::string lowercase(std::string_view value) {
      std::string out(value);
      for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      }
      return out;
    }

    // base64url without padding: url- and cookie-safe, so the SPA can put the
    // value in a header verbatim.
    [[nodiscard]] std::string base64url_encode(const unsigned char* data, std::size_t len) {
      std::string out;
      out.reserve(((len + 2) / 3) * 4);

      const std::size_t full_triples = len / 3;
      for (std::size_t triple = 0; triple < full_triples; ++triple) {
        const std::size_t offset = triple * 3;
        const std::uint32_t chunk = (static_cast<std::uint32_t>(data[offset]) << 16U) |
                                    (static_cast<std::uint32_t>(data[offset + 1]) << 8U) |
                                    static_cast<std::uint32_t>(data[offset + 2]);
        out.push_back(k_base64url_alphabet[(chunk >> 18U) & 0x3FU]);
        out.push_back(k_base64url_alphabet[(chunk >> 12U) & 0x3FU]);
        out.push_back(k_base64url_alphabet[(chunk >> 6U) & 0x3FU]);
        out.push_back(k_base64url_alphabet[chunk & 0x3FU]);
      }

      const std::size_t tail = full_triples * 3;
      const std::size_t rest = len - tail;
      if (rest == 1) {
        const std::uint32_t chunk = static_cast<std::uint32_t>(data[tail]) << 16U;
        out.push_back(k_base64url_alphabet[(chunk >> 18U) & 0x3FU]);
        out.push_back(k_base64url_alphabet[(chunk >> 12U) & 0x3FU]);
      } else if (rest == 2) {
        const std::uint32_t chunk = (static_cast<std::uint32_t>(data[tail]) << 16U) |
                                    (static_cast<std::uint32_t>(data[tail + 1]) << 8U);
        out.push_back(k_base64url_alphabet[(chunk >> 18U) & 0x3FU]);
        out.push_back(k_base64url_alphabet[(chunk >> 12U) & 0x3FU]);
        out.push_back(k_base64url_alphabet[(chunk >> 6U) & 0x3FU]);
      }
      return out;
    }

    [[nodiscard]] bool is_insecure_cookie_flag_set() {
      const char* flag = std::getenv("FMGR_DEV_INSECURE_COOKIES");
      return flag != nullptr && std::string_view(flag) == "1";
    }

    [[nodiscard]] bool is_production() {
      const char* env = std::getenv("FMGR_ENV");
      return env != nullptr && std::string_view(env) == "production";
    }

    // Safe methods cannot change state, so there is nothing for a cross-site
    // request to forge.
    [[nodiscard]] bool is_safe_method(std::string_view method) {
      return method == "GET" || method == "HEAD" || method == "OPTIONS";
    }

    // "https://host:port" -> "host:port". The scheme is dropped on purpose: the
    // gateway usually sits behind a TLS-terminating proxy, so it cannot tell
    // http from https from the request alone, while the authority is what
    // decides whether an origin is this site at all. A page that can claim the
    // victim's authority is already same-origin.
    [[nodiscard]] std::string origin_authority(std::string_view origin) {
      const auto scheme_end = origin.find("://");
      std::string_view rest =
          scheme_end == std::string_view::npos ? origin : origin.substr(scheme_end + 3);
      const auto path_start = rest.find('/');
      if (path_start != std::string_view::npos) {
        rest = rest.substr(0, path_start);
      }
      return lowercase(rest);
    }

    [[nodiscard]] std::string normalize_origin(std::string_view origin) {
      std::string out = lowercase(origin);
      while (!out.empty() && out.back() == '/') {
        out.pop_back();
      }
      return out;
    }

  } // namespace

  BrowserSessionConfig BrowserSessionConfig::from_env() {
    BrowserSessionConfig config;
    config.secure_cookies = !is_insecure_cookie_flag_set();
    if (const char* origin = std::getenv("FMGR_WEB_ORIGIN"); origin != nullptr) {
      config.web_origin = normalize_origin(origin);
    }
    return config;
  }

  std::string validate_browser_session_env() {
    if (!is_insecure_cookie_flag_set() || !is_production()) {
      return {};
    }
    return "FMGR_DEV_INSECURE_COOKIES=1 drops the Secure flag from the browser session "
           "cookies and must not be combined with FMGR_ENV=production";
  }

  std::string generate_csrf_token() {
    std::array<unsigned char, k_csrf_token_bytes> buffer{};
    randombytes_buf(buffer.data(), buffer.size());
    return base64url_encode(buffer.data(), buffer.size());
  }

  drogon::Cookie session_cookie(std::string token, const BrowserSessionConfig& config) {
    drogon::Cookie cookie{std::string(k_session_cookie_name), std::move(token)};
    cookie.setPath(std::string(k_session_cookie_path));
    cookie.setHttpOnly(true);
    cookie.setSecure(config.secure_cookies);
    cookie.setSameSite(drogon::Cookie::SameSite::kStrict);
    // No Max-Age: the session cookie lives until the browser closes or the server
    // expires the session, so a revoked session cannot be kept alive client-side.
    return cookie;
  }

  drogon::Cookie csrf_cookie(std::string token, const BrowserSessionConfig& config) {
    drogon::Cookie cookie{std::string(k_csrf_cookie_name), std::move(token)};
    cookie.setPath(std::string(k_csrf_cookie_path));
    cookie.setHttpOnly(false);
    cookie.setSecure(config.secure_cookies);
    cookie.setSameSite(drogon::Cookie::SameSite::kStrict);
    return cookie;
  }

  drogon::Cookie expired_session_cookie(const BrowserSessionConfig& config) {
    auto cookie = session_cookie("", config);
    cookie.setExpiresDate(trantor::Date(0));
    cookie.setMaxAge(0);
    return cookie;
  }

  drogon::Cookie expired_csrf_cookie(const BrowserSessionConfig& config) {
    auto cookie = csrf_cookie("", config);
    cookie.setExpiresDate(trantor::Date(0));
    cookie.setMaxAge(0);
    return cookie;
  }

  BrowserRequest browser_request_from(const drogon::HttpRequest& req) {
    BrowserRequest out;
    out.method = std::string(drogon::to_string_view(req.method()));
    out.authorization = req.getHeader("authorization");
    // Cookie lookup is Drogon's, deliberately. This module used to carry its own
    // `parse_cookie_header` ("later duplicates win") — dead code, since nothing
    // here called it, and the security review filed it as exactly that. Reading
    // the two cookies by name is also the safer default: a duplicate `fmgr_csrf`
    // cannot help an attacker, because the double-submit check compares the
    // header against whichever value the *browser* would have sent, and the
    // `Origin` check above runs first and independently of any cookie. If a
    // future change needs control over duplicate-cookie resolution, bring the
    // parser back together with the caller that depends on it.
    out.session_cookie = req.getCookie(std::string(k_session_cookie_name));
    out.csrf_cookie = req.getCookie(std::string(k_csrf_cookie_name));
    out.csrf_header = req.getHeader(std::string(k_csrf_header_name));
    out.origin = req.getHeader("origin");
    out.host = req.getHeader("host");
    return out;
  }

  std::string authorization_metadata(const BrowserRequest& req) {
    if (!req.authorization.empty()) {
      return req.authorization;
    }
    if (!req.session_cookie.empty()) {
      return "Bearer " + req.session_cookie;
    }
    return {};
  }

  std::optional<std::string> csrf_denial(const BrowserRequest& req,
                                         const BrowserSessionConfig& config) {
    if (is_safe_method(req.method)) {
      return std::nullopt;
    }
    // A bearer token is not ambient: no other site can make the browser attach
    // it, so the request cannot be a cross-site forgery.
    if (!req.authorization.empty()) {
      return std::nullopt;
    }

    if (!req.origin.empty()) {
      const std::string authority = origin_authority(req.origin);
      const bool host_matches = !req.host.empty() && authority == lowercase(req.host);
      const bool configured_matches =
          !config.web_origin.empty() && normalize_origin(req.origin) == config.web_origin;
      if (!host_matches && !configured_matches) {
        return "request origin " + req.origin + " is neither the request host nor FMGR_WEB_ORIGIN";
      }
    }

    if (!req.session_cookie.empty() &&
        (req.csrf_cookie.empty() || req.csrf_header != req.csrf_cookie)) {
      return "a cookie-authenticated " + req.method + " must echo the fmgr_csrf cookie in " +
             std::string(k_csrf_header_name);
    }
    return std::nullopt;
  }

} // namespace fmgr::rest
