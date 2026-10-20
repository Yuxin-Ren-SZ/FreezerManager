// SPDX-License-Identifier: AGPL-3.0-or-later

#include "rest/StaticAssets.h"

#include <drogon/drogon.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <system_error>

namespace fmgr::rest {
  namespace {

    // The exact value pinned by TODO.md G0.3. No 'unsafe-inline': the Vite
    // bundle is external JS/CSS, and inline script/style would only widen the
    // attack surface here.
    constexpr std::string_view kContentSecurityPolicy =
        "default-src 'self'; frame-ancestors 'none'; base-uri 'none'; object-src 'none'";
    // Vite fingerprints every file under /assets/ with a content hash, so a
    // year-long immutable cache is safe: a new build produces new names.
    constexpr std::string_view kImmutableCache = "public, max-age=31536000, immutable";
    // HTML must be revalidated on every load, or a deploy keeps serving the old
    // shell (and the old asset names) until the cache expires.
    constexpr std::string_view kRevalidateCache = "no-cache";

    // Drogon routes exact handlers case-insensitively (it lowercases the lookup
    // path), so an API caller can reach a gateway route with /API/V1/... in the
    // URL while `req->path()` keeps the caller's case. The fallback and the
    // header advice must classify it the same way the router did.
    [[nodiscard]] bool starts_with_ci(std::string_view path, std::string_view prefix) {
      if (path.size() < prefix.size()) {
        return false;
      }
      return std::equal(prefix.begin(), prefix.end(), path.begin(), [](char expected, char actual) {
        return expected == static_cast<char>(std::tolower(static_cast<unsigned char>(actual)));
      });
    }

    [[nodiscard]] std::size_t parse_body_limit(const char* raw) {
      if (raw == nullptr || *raw == '\0') {
        return StaticAssetsConfig::kDefaultMaxBodyBytes;
      }
      const std::string text(raw);
      if (text.find_first_not_of("0123456789") != std::string::npos) {
        throw std::invalid_argument("FMGR_REST_MAX_BODY_BYTES must be a positive integer: " + text);
      }
      try {
        const auto value = std::stoull(text);
        if (value == 0) {
          throw std::invalid_argument("FMGR_REST_MAX_BODY_BYTES must be greater than zero");
        }
        return static_cast<std::size_t>(value);
      } catch (const std::out_of_range&) {
        throw std::invalid_argument("FMGR_REST_MAX_BODY_BYTES is out of range: " + text);
      }
    }

    // `path` is already percent-decoded by Drogon, so `%2e%2e` has become `..`
    // here. Refusing the dangerous shapes before touching the filesystem keeps
    // the canonical check below a second line of defence rather than the only
    // one. A backslash is not a POSIX separator, but rejecting it keeps the
    // rule portable.
    [[nodiscard]] bool has_unsafe_segment(std::string_view path) {
      if (path.empty() || path.front() != '/') {
        return true;
      }
      if (path.find('\0') != std::string_view::npos) {
        return true;
      }
      std::string_view rest = path.substr(1);
      while (true) {
        const auto slash = rest.find('/');
        const auto segment = rest.substr(0, slash);
        if (segment == ".." || segment == "." || segment.find('\\') != std::string_view::npos) {
          return true;
        }
        if (slash == std::string_view::npos) {
          return false;
        }
        rest.remove_prefix(slash + 1);
      }
    }

    // Resolve `candidate` and refuse it unless it stays inside the canonical
    // `root`. weakly_canonical resolves symlinks, so a link inside the root
    // that points outside it is refused too.
    [[nodiscard]] std::optional<std::filesystem::path>
    canonical_within(const std::filesystem::path& root, const std::filesystem::path& candidate) {
      std::error_code error;
      const auto resolved = std::filesystem::weakly_canonical(candidate, error);
      if (error) {
        return std::nullopt;
      }
      const auto relative = resolved.lexically_relative(root);
      if (relative.empty() || relative.is_absolute()) {
        return std::nullopt;
      }
      const auto first = relative.begin();
      if (first != relative.end() && *first == "..") {
        return std::nullopt;
      }
      return resolved;
    }

    // The SPA handler. Registered for every method because an unknown /api/*
    // path must be a JSON 404 no matter what verb reaches it; only GET serves
    // files, and exact gateway routes are matched before a regex ever runs.
    void register_handler(const std::filesystem::path& root) {
      drogon::app().registerHandlerViaRegex(
          "^/.*$", [root](const drogon::HttpRequestPtr& req,
                          std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            const std::string& path = req->path();
            if (starts_with_ci(path, "/api/")) {
              // The gateway's own routes are exact matches and never reach
              // here. An unknown /api/* path is still an API caller: answer in
              // JSON, not with the SPA shell that would look like a 200.
              auto resp = drogon::HttpResponse::newHttpResponse();
              resp->setStatusCode(drogon::k404NotFound);
              resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
              resp->setBody(
                  nlohmann::json{{"code", "NOT_FOUND"}, {"message", "unknown API endpoint"}}
                      .dump());
              callback(resp);
              return;
            }
            if (req->method() != drogon::Get) {
              callback(drogon::HttpResponse::newNotFoundResponse(req));
              return;
            }
            const auto resolved = StaticAssets::resolve_path(root, path);
            if (!resolved.has_value()) {
              callback(drogon::HttpResponse::newNotFoundResponse(req));
              return;
            }
            std::error_code error;
            if (!std::filesystem::is_regular_file(*resolved, error)) {
              callback(drogon::HttpResponse::newNotFoundResponse(req));
              return;
            }
            callback(drogon::HttpResponse::newFileResponse(resolved->string()));
          });
    }

    // Post-handling advice: RestGateway.cc stays unaware of the SPA (G0.3).
    void register_advice(bool tls) {
      drogon::app().registerPostHandlingAdvice(
          [tls](const drogon::HttpRequestPtr& req, const drogon::HttpResponsePtr& resp) {
            resp->addHeader("Content-Security-Policy", std::string(kContentSecurityPolicy));
            resp->addHeader("X-Content-Type-Options", "nosniff");
            resp->addHeader("Referrer-Policy", "no-referrer");
            if (tls) {
              resp->addHeader("Strict-Transport-Security", "max-age=31536000");
            }
            const std::string& path = req->path();
            if (starts_with_ci(path, "/api/")) {
              // API responses carry PHI and credentials: never on disk, never
              // in a shared cache, not even after a browser back-navigation.
              resp->addHeader("Cache-Control", "no-store");
              return;
            }
            const bool immutable = starts_with_ci(path, "/assets/") &&
                                   resp->getStatusCode() == drogon::k200OK &&
                                   resp->contentType() != drogon::CT_TEXT_HTML;
            resp->addHeader("Cache-Control",
                            std::string(immutable ? kImmutableCache : kRevalidateCache));
          });
    }

  } // namespace

  StaticAssetsConfig StaticAssetsConfig::from_env(bool tls) {
    StaticAssetsConfig config;
    config.tls = tls;
    if (const char* root = std::getenv("FMGR_WEB_ROOT"); root != nullptr && *root != '\0') {
      config.web_root = root;
    }
    config.max_body_bytes = parse_body_limit(std::getenv("FMGR_REST_MAX_BODY_BYTES"));
    return config;
  }

  void StaticAssets::install(const StaticAssetsConfig& config) {
    // The body limit is independent of the SPA: a deployment without
    // FMGR_WEB_ROOT still imports CSVs through /api/v1/sample/import and needs
    // the same ~10 MiB ceiling the gRPC listener already has (C-10).
    drogon::app().setClientMaxBodySize(config.max_body_bytes);
    if (!config.serves_spa()) {
      return;
    }

    std::error_code error;
    const auto root = std::filesystem::weakly_canonical(config.web_root, error);
    if (error || !std::filesystem::is_directory(root, error)) {
      throw std::runtime_error("FMGR_WEB_ROOT is not a directory: " + config.web_root.string());
    }
    register_handler(root);
    register_advice(config.tls);
  }

  std::optional<std::filesystem::path>
  StaticAssets::resolve_path(const std::filesystem::path& web_root, std::string_view decoded_path) {
    if (has_unsafe_segment(decoded_path)) {
      return std::nullopt;
    }

    std::error_code error;
    const auto root = std::filesystem::weakly_canonical(web_root, error);
    if (error) {
      return std::nullopt;
    }

    std::filesystem::path relative;
    std::string_view rest = decoded_path.substr(1);
    while (!rest.empty()) {
      const auto slash = rest.find('/');
      const auto segment = rest.substr(0, slash);
      if (!segment.empty()) {
        relative /= std::string(segment);
      }
      if (slash == std::string_view::npos) {
        break;
      }
      rest.remove_prefix(slash + 1);
    }

    if (!relative.empty()) {
      const auto candidate = canonical_within(root, root / relative);
      if (!candidate.has_value()) {
        // The path escaped the root (a symlink, or a canonicalisation error).
        // Refuse it; falling back to index.html would hide the attempt.
        return std::nullopt;
      }
      std::error_code file_error;
      if (std::filesystem::is_regular_file(*candidate, file_error)) {
        return candidate;
      }
    }
    // Not a file: a client-side route. index.html is resolved through the same
    // containment check, so a web root whose index is a symlink outside the
    // root is refused rather than served.
    return canonical_within(root, root / "index.html");
  }

} // namespace fmgr::rest
