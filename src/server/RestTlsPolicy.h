// SPDX-License-Identifier: AGPL-3.0-or-later

// REST listener TLS policy (review C6). The gRPC listener already refuses to
// start plaintext when FMGR_REQUIRE_TLS is set (FreezerServer::build), but the
// REST listener historically fell back to plaintext silently. This makes the
// REST guard symmetric: under FMGR_REQUIRE_TLS a non-loopback REST bind must be
// TLS (or bound to loopback for a reverse-proxy to terminate TLS), and a
// half-configured cert/key pair is rejected in any mode.
//
// Pure and header-only so main.cc fails fast before binding any port and the
// rules are unit-testable without a live listener.
#ifndef FMGR_SERVER_RESTTLSPOLICY_H
#define FMGR_SERVER_RESTTLSPOLICY_H

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace fmgr::server {

  // True for addresses that are only reachable from the local host, where
  // serving plaintext (behind a reverse proxy that terminates TLS) is
  // acceptable. "0.0.0.0" / "::" / "" bind all interfaces and are NOT loopback.
  [[nodiscard]] inline bool is_loopback_host(std::string_view host) {
    return host == "127.0.0.1" || host == "::1" || host == "localhost";
  }

  struct RestTlsConfig {
    std::string host;
    std::uint16_t port{0};
    bool has_cert{false};
    bool has_key{false};
    bool require_tls{false};
  };

  // Throw std::invalid_argument when the REST TLS configuration violates policy.
  // Returns normally when the configuration is acceptable.
  inline void validate_rest_tls_policy(const RestTlsConfig& cfg) {
    // Half-configured cert/key is always an error: it silently degraded to
    // plaintext before, hiding a misconfiguration.
    if (cfg.has_cert != cfg.has_key) {
      throw std::invalid_argument(
          "REST TLS requires both FMGR_REST_TLS_CERT and FMGR_REST_TLS_KEY");
    }

    if (!cfg.require_tls) {
      return;
    }

    const bool rest_tls = cfg.has_cert && cfg.has_key;
    if (!rest_tls && !is_loopback_host(cfg.host)) {
      throw std::invalid_argument(
          "FMGR_REQUIRE_TLS=1 but REST listener " + cfg.host + ":" + std::to_string(cfg.port) +
          " has no TLS cert/key; set FMGR_REST_TLS_CERT and FMGR_REST_TLS_KEY or bind to loopback");
    }
  }

} // namespace fmgr::server

#endif // FMGR_SERVER_RESTTLSPOLICY_H
