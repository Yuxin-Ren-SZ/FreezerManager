// SPDX-License-Identifier: AGPL-3.0-or-later

// Shared harness for the SPA static-asset integration tests (TODO.md G0.3).
//
// Drogon's HTTP application is a process-wide singleton, so each startup
// configuration case needs its own test executable: SPA served on plaintext,
// SPA served with TLS and a lowered body limit, and static serving disabled.
// The helpers here keep the three binaries to their configuration and
// assertions.
#ifndef FMGR_TESTS_INTEGRATION_REST_STATIC_ASSETS_HARNESS_H
#define FMGR_TESTS_INTEGRATION_REST_STATIC_ASSETS_HARNESS_H

#include <drogon/HttpClient.h>
#include <drogon/HttpRequest.h>
#include <drogon/drogon.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace fmgr::test::static_assets {

  inline constexpr std::string_view kIndexHtml =
      "<!doctype html><html><body><div id=\"root\">spa-index</div></body></html>";
  inline constexpr std::string_view kHashedAsset = "console.log('hashed-asset');";
  inline constexpr std::string_view kSecret = "TOP-SECRET-TRAVERSAL-CANARY";

  // Give each Drogon integration binary its own current directory. Drogon
  // derives rootPath and uploads/tmp from cwd when its singleton is created.
  struct WorkingDirectory {
    std::filesystem::path original;
    std::filesystem::path path;

    void create(std::string_view name) {
      original = std::filesystem::current_path();
      path =
          std::filesystem::temp_directory_path() /
          ("fmgr-rest-static-assets-cwd-" + std::string(name) + "-" + std::to_string(::getpid()));
      std::error_code error;
      std::filesystem::remove_all(path, error);
      std::filesystem::create_directories(path);
      std::filesystem::current_path(path);
    }

    void restore_and_remove() {
      if (path.empty()) {
        return;
      }
      std::filesystem::current_path(original);
      std::error_code error;
      std::filesystem::remove_all(path, error);
      path.clear();
    }
  };

  [[nodiscard]] inline bool has_upload_temp_files() {
    const auto temp = std::filesystem::current_path() / "uploads" / "tmp";
    std::error_code error;
    if (!std::filesystem::is_directory(temp, error)) {
      return false;
    }
    for (std::filesystem::recursive_directory_iterator it(temp, error), end; it != end && !error;
         it.increment(error)) {
      if (it->is_regular_file(error)) {
        return true;
      }
    }
    return false;
  }

  // A throwaway Vite-like web root: index.html plus one hashed asset under
  // /assets. A canary file next to the root proves traversal never reaches it,
  // and a symlink inside the root exercises the canonical containment check.
  struct WebRoot {
    std::filesystem::path base;
    std::filesystem::path root;
    std::filesystem::path secret;

    [[nodiscard]] static WebRoot create(std::string_view name) {
      WebRoot web;
      web.base =
          std::filesystem::temp_directory_path() /
          ("fmgr-rest-static-assets-" + std::string(name) + "-" + std::to_string(::getpid()));
      web.root = web.base / "web";
      web.secret = web.base / "secret.txt";
      std::error_code ec;
      std::filesystem::remove_all(web.base, ec);
      std::filesystem::create_directories(web.root / "assets");
      write(web.root / "index.html", kIndexHtml);
      write(web.root / "assets" / "app-abc123.js", kHashedAsset);
      write(web.secret, kSecret);
      std::filesystem::create_symlink(web.secret, web.root / "escape.txt");
      return web;
    }

    void remove() const {
      std::error_code ec;
      std::filesystem::remove_all(base, ec);
    }

  private:
    static void write(const std::filesystem::path& path, std::string_view contents) {
      std::ofstream out(path, std::ios::binary);
      out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    }
  };

  // Ask the OS for a free loopback port, then hand it to Drogon. The tiny race
  // between close() and bind() is acceptable in these tests.
  [[nodiscard]] inline std::uint16_t find_free_port() {
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

  inline void wait_until_running() {
    for (int i = 0; i < 500 && !drogon::app().getLoop()->isRunning(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

  struct HttpResult {
    drogon::ReqResult transport{drogon::ReqResult::NetworkFailure};
    int status{0};
    std::string body;
    std::string content_type;
    std::string cache_control;
    std::string csp;
    std::string nosniff;
    std::string referrer_policy;
    std::string hsts;
    std::string allow_origin;

    [[nodiscard]] bool ok() const {
      return transport == drogon::ReqResult::Ok;
    }
  };

  // `path` is sent byte for byte (`setPathEncode(false)`): the percent-encoded
  // traversal cases must reach the server exactly as written, and Drogon decodes
  // the URL path server-side — which is the layer under test.
  [[nodiscard]] inline HttpResult request(const std::uint16_t port, drogon::HttpMethod method,
                                          std::string_view path, const std::string& body = {},
                                          std::string_view origin = {}) {
    auto client = drogon::HttpClient::newHttpClient("127.0.0.1", port);
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setPathEncode(false);
    req->setMethod(method);
    req->setPath(std::string(path));
    if (!origin.empty()) {
      req->addHeader("Origin", std::string(origin));
    }
    if (!body.empty()) {
      req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
      req->setBody(body);
    }
    auto [result, resp] = client->sendRequest(req, 15.0);
    HttpResult out;
    out.transport = result;
    if (resp == nullptr) {
      return out;
    }
    out.status = resp->getStatusCode();
    out.body = std::string(resp->body());
    out.content_type = resp->getHeader("content-type");
    out.cache_control = resp->getHeader("cache-control");
    out.csp = resp->getHeader("content-security-policy");
    out.nosniff = resp->getHeader("x-content-type-options");
    out.referrer_policy = resp->getHeader("referrer-policy");
    out.hsts = resp->getHeader("strict-transport-security");
    out.allow_origin = resp->getHeader("access-control-allow-origin");
    return out;
  }

  [[nodiscard]] inline HttpResult get(const std::uint16_t port, const std::string& path) {
    return request(port, drogon::Get, path);
  }

  [[nodiscard]] inline HttpResult post(const std::uint16_t port, const std::string& path,
                                       const std::string& body) {
    return request(port, drogon::Post, path, body);
  }

} // namespace fmgr::test::static_assets

#endif // FMGR_TESTS_INTEGRATION_REST_STATIC_ASSETS_HARNESS_H
