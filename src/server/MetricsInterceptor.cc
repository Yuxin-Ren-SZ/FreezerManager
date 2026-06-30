// SPDX-License-Identifier: AGPL-3.0-or-later

#include "server/MetricsInterceptor.h"

#include "obs/Metrics.h"

#include <grpcpp/grpcpp.h>

#include <chrono>
#include <string>

namespace fmgr::server {
  namespace {

    [[nodiscard]] std::string status_code_name(grpc::StatusCode code) {
      switch (code) {
      case grpc::StatusCode::OK:
        return "OK";
      case grpc::StatusCode::CANCELLED:
        return "CANCELLED";
      case grpc::StatusCode::UNKNOWN:
        return "UNKNOWN";
      case grpc::StatusCode::INVALID_ARGUMENT:
        return "INVALID_ARGUMENT";
      case grpc::StatusCode::DEADLINE_EXCEEDED:
        return "DEADLINE_EXCEEDED";
      case grpc::StatusCode::NOT_FOUND:
        return "NOT_FOUND";
      case grpc::StatusCode::ALREADY_EXISTS:
        return "ALREADY_EXISTS";
      case grpc::StatusCode::PERMISSION_DENIED:
        return "PERMISSION_DENIED";
      case grpc::StatusCode::UNAUTHENTICATED:
        return "UNAUTHENTICATED";
      case grpc::StatusCode::RESOURCE_EXHAUSTED:
        return "RESOURCE_EXHAUSTED";
      case grpc::StatusCode::FAILED_PRECONDITION:
        return "FAILED_PRECONDITION";
      case grpc::StatusCode::ABORTED:
        return "ABORTED";
      case grpc::StatusCode::OUT_OF_RANGE:
        return "OUT_OF_RANGE";
      case grpc::StatusCode::UNIMPLEMENTED:
        return "UNIMPLEMENTED";
      case grpc::StatusCode::INTERNAL:
        return "INTERNAL";
      case grpc::StatusCode::UNAVAILABLE:
        return "UNAVAILABLE";
      case grpc::StatusCode::DATA_LOSS:
        return "DATA_LOSS";
      default:
        return "UNKNOWN";
      }
    }

  } // namespace

  void MetricsInterceptor::Intercept(grpc::experimental::InterceptorBatchMethods* methods) {
    using grpc::experimental::InterceptionHookPoints;
    // PRE_SEND_STATUS fires once as the handler completes (for streaming RPCs,
    // when the stream closes), and is where the final status is available.
    if (methods->QueryInterceptionHookPoint(InterceptionHookPoints::PRE_SEND_STATUS)) {
      const auto code = status_code_name(methods->GetSendStatus().error_code());
      obs::metrics().inc_counter("rpc_requests_total", "Total gRPC requests by method and code.",
                                 {{"method", method_}, {"code", code}});
      if (unary_) {
        // Latency is only meaningful for unary RPCs; a server-stream's lifetime is
        // bounded by the client, not the handler, so recording it would skew the
        // histogram.
        const auto elapsed = std::chrono::steady_clock::now() - start_;
        const double seconds = std::chrono::duration<double>(elapsed).count();
        obs::metrics().observe_latency("rpc_latency_seconds", "Unary gRPC handler latency.",
                                       {{"method", method_}}, seconds);
      }
    }
    methods->Proceed();
  }

  grpc::experimental::Interceptor*
  MetricsInterceptorFactory::CreateServerInterceptor(grpc::experimental::ServerRpcInfo* info) {
    const bool unary = info->type() == grpc::experimental::ServerRpcInfo::Type::UNARY;
    return new MetricsInterceptor(info->method(), unary);
  }

} // namespace fmgr::server
