// SPDX-License-Identifier: AGPL-3.0-or-later

#include "server/RateLimitInterceptor.h"

#include "rpc/AuthMiddleware.h"

#include <string_view>

namespace fmgr::server {

  RateLimitInterceptor::RateLimitInterceptor(RateLimitOptions opts)
      : opts_(opts), data_limiter_(opts.data) {
    if (opts_.enabled) {
      rpc::AuthMiddleware::set_process_data_rate_limiter(&data_limiter_);
      installed_ = true;
    }
  }

  RateLimitInterceptor::~RateLimitInterceptor() {
    if (installed_) {
      // Uninstall before data_limiter_ dies so no in-flight authorize() dereferences a
      // dangling gate.
      rpc::AuthMiddleware::set_process_data_rate_limiter(nullptr);
    }
  }

  bool RateLimitInterceptor::is_exempt_method(std::string_view method) {
    return method.starts_with("/grpc.health.v1.Health/") ||
           method.starts_with("/grpc.reflection.");
  }

} // namespace fmgr::server
