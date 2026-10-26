// SPDX-License-Identifier: AGPL-3.0-or-later

// Serving the built SPA from freezerd (TODO.md G0.3).
//
// The browser SPA is same-origin only (G-arch 5): the REST gateway deliberately
// sends no CORS headers, so a deployment is either one process serving both the
// API and the static bundle, or a reverse proxy doing the same in front of it.
// This module is the first option — set `FMGR_WEB_ROOT` to the Vite `dist/`
// directory and the REST listener serves it too.
//
// Routing rules:
//   * `/api/*` is never shadowed: exact gateway routes win, and an unknown
//     `/api/*` path gets a JSON 404, not the SPA shell.
//   * `/healthz` and `/metrics` keep their own exact handlers.
//   * Every other GET is a client-side route: serve the file when one exists
//     under the web root, otherwise `index.html`.
//   * Traversal is refused before the filesystem is touched (`..`, `.`,
//     backslashes, NUL bytes) and again by canonical containment, which also
//     catches a symlink that points outside the root.
//
// Browser security headers are applied by a Drogon post-handling advice, so
// RestGateway.cc stays unaware of the SPA. The advice is registered only when
// `FMGR_WEB_ROOT` is set: with it unset the process behaves exactly as before.
#ifndef FMGR_REST_STATICASSETS_H
#define FMGR_REST_STATICASSETS_H

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace fmgr::rest {

  struct StaticAssetsConfig {
    // Matches FreezerServerOptions::max_receive_message_bytes (C-10).
    static constexpr std::size_t k_default_max_body_bytes = std::size_t{10} * 1024 * 1024;

    // Vite `dist/` directory. Empty disables static serving entirely: no
    // fallback handler and no security-header advice are registered.
    std::filesystem::path web_root;
    // Request bodies larger than this are rejected by Drogon with 413. The
    // default matches FreezerServerOptions::max_receive_message_bytes (C-10),
    // so a CSV import that fits the gRPC inbound cap also fits the HTTP front
    // door instead of dying at Drogon's 1 MiB default.
    std::size_t max_body_bytes{k_default_max_body_bytes};
    // True when the REST listener terminates TLS; HSTS is only advertised there.
    bool tls{false};

    [[nodiscard]] bool serves_spa() const noexcept {
      return !web_root.empty();
    }

    // Samples FMGR_WEB_ROOT and FMGR_REST_MAX_BODY_BYTES once, at startup.
    // An unset or empty FMGR_WEB_ROOT means "disabled". A body limit that is not
    // a positive decimal integer throws std::invalid_argument rather than
    // silently falling back to a different limit than the operator configured.
    [[nodiscard]] static StaticAssetsConfig from_env(bool tls);
  };

  class StaticAssets {
  public:
    // Applies `max_body_bytes` to the process-wide Drogon app and, when
    // `config.serves_spa()`, registers the SPA fallback plus the security-header
    // advice. Call once, before drogon::app().run(). Throws std::runtime_error
    // when the configured web root is not a directory; a deployment that points
    // at the wrong path should fail at startup, not 404 every page.
    static void install(const StaticAssetsConfig& config);

    // Maps one percent-decoded URL path to a file under `web_root`. A path that
    // names a regular file is served as-is; anything else (a client-side route)
    // falls back to `<web_root>/index.html`. std::nullopt means "refuse": a
    // `..`/`.` segment, a backslash, a NUL byte, or a canonical path that
    // escapes the root (including through a symlink). Paths are returned
    // canonical so the caller opens exactly what was checked.
    [[nodiscard]] static std::optional<std::filesystem::path>
    resolve_path(const std::filesystem::path& web_root, std::string_view decoded_path);
  };

} // namespace fmgr::rest

#endif // FMGR_REST_STATICASSETS_H
