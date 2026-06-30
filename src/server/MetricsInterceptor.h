// SPDX-License-Identifier: AGPL-3.0-or-later

// gRPC server interceptor that feeds the obs::metrics() registry (PRD §17):
// per-method request counts labelled by status code, plus a latency histogram
// for unary RPCs. Instrumenting at the transport layer keeps every handler free
// of metrics code and avoids double-counting REST calls, which reach the same
// handlers over the in-process channel.
#ifndef FMGR_SERVER_METRICSINTERCEPTOR_H
#define FMGR_SERVER_METRICSINTERCEPTOR_H

#include <grpcpp/support/interceptor.h>
#include <grpcpp/support/server_interceptor.h>

#include <chrono>
#include <string>

namespace fmgr::server {

  class MetricsInterceptor : public grpc::experimental::Interceptor {
  public:
    MetricsInterceptor(std::string method, bool unary)
        : method_(std::move(method)), unary_(unary), start_(std::chrono::steady_clock::now()) {}

    void Intercept(grpc::experimental::InterceptorBatchMethods* methods) override;

  private:
    std::string method_;
    bool unary_;
    std::chrono::steady_clock::time_point start_;
  };

  class MetricsInterceptorFactory : public grpc::experimental::ServerInterceptorFactoryInterface {
  public:
    grpc::experimental::Interceptor*
    CreateServerInterceptor(grpc::experimental::ServerRpcInfo* info) override;
  };

} // namespace fmgr::server

#endif // FMGR_SERVER_METRICSINTERCEPTOR_H
