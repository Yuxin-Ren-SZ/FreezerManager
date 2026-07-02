// SPDX-License-Identifier: AGPL-3.0-or-later

// Global request throttle for the gRPC surface (security audit C-10 / DoS).
//
// gRPC C++ server interceptors cannot reject an RPC with a chosen status code
// (Hijack is client-side only), so this "interceptor" enforces at the single
// choke point every authenticated handler already passes through:
// rpc::AuthMiddleware::authorize(). Constructing a RateLimitInterceptor installs
// its data-tier limiter as the process gate consulted there; destroying it
// uninstalls (RAII), so throttling is scoped to one server's lifetime and unit
// tests that never build a server are unaffected.
//
// Two tiers, both configurable via FreezerServerOptions:
//   - auth tier: unauthenticated Login/SubmitMfa, throttled per source IP with a
//     higher burst. Owned per-service by AuthServiceImpl; this class only carries
//     the config so the server can hand it over.
//   - data tier: authenticated RPCs, throttled per bearer token with a lower
//     burst, via the installed process gate.
//
// Health/metrics endpoints never reach authorize(), so they are exempt by
// construction; is_exempt_method() documents that contract for reflection/health
// gRPC methods a future direct-interceptor wiring might see.
#ifndef FMGR_SERVER_RATELIMITINTERCEPTOR_H
#define FMGR_SERVER_RATELIMITINTERCEPTOR_H

#include "rpc/RateLimiter.h"

#include <string_view>

namespace fmgr::server {

  struct RateLimitOptions {
    // When false, no gate is installed and all RPCs pass unthrottled.
    bool enabled{true};
    // Auth endpoints: unauthenticated, per-source-IP, higher burst.
    rpc::RateLimiterConfig auth{.capacity = 30.0, .refill_per_sec = 5.0};
    // Data endpoints: authenticated, per-bearer, lower sustained rate.
    rpc::RateLimiterConfig data{.capacity = 120.0, .refill_per_sec = 40.0};
  };

  class RateLimitInterceptor {
  public:
    explicit RateLimitInterceptor(RateLimitOptions opts);
    ~RateLimitInterceptor();

    RateLimitInterceptor(const RateLimitInterceptor&) = delete;
    RateLimitInterceptor& operator=(const RateLimitInterceptor&) = delete;
    RateLimitInterceptor(RateLimitInterceptor&&) = delete;
    RateLimitInterceptor& operator=(RateLimitInterceptor&&) = delete;

    // Auth-tier bucket config, handed to AuthServiceImpl's per-IP login limiter.
    [[nodiscard]] const rpc::RateLimiterConfig& auth_config() const {
      return opts_.auth;
    }

    // True for gRPC methods that must never be throttled (health, reflection).
    [[nodiscard]] static bool is_exempt_method(std::string_view method);

  private:
    RateLimitOptions opts_;
    rpc::RateLimiter data_limiter_;
    bool installed_{false};
  };

} // namespace fmgr::server

#endif // FMGR_SERVER_RATELIMITINTERCEPTOR_H
