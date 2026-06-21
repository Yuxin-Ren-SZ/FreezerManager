// SPDX-License-Identifier: AGPL-3.0-or-later

// Maps a gRPC status (returned by the in-process service handlers) to an HTTP
// status code + a JSON error body. This is the reverse direction of
// server/GrpcErrorTranslation.h, which maps domain exceptions to gRPC codes.
#ifndef FMGR_REST_RESTERRORTRANSLATION_H
#define FMGR_REST_RESTERRORTRANSLATION_H

#include <grpcpp/support/status.h>
#include <nlohmann/json.hpp>

#include <string>

namespace fmgr::rest {

  struct HttpError {
    int status_code;  // HTTP status
    std::string body; // JSON: {"code": "<GRPC_CODE>", "message": "..."}
  };

  // HTTP status for a gRPC code. The mapping follows the conventional
  // grpc-gateway translation so existing client tooling behaves as expected.
  [[nodiscard]] inline int http_status_for(grpc::StatusCode code) {
    switch (code) {
    case grpc::StatusCode::OK:
      return 200;
    case grpc::StatusCode::INVALID_ARGUMENT:
    case grpc::StatusCode::OUT_OF_RANGE:
      return 400;
    case grpc::StatusCode::UNAUTHENTICATED:
      return 401;
    case grpc::StatusCode::PERMISSION_DENIED:
      return 403;
    case grpc::StatusCode::NOT_FOUND:
      return 404;
    case grpc::StatusCode::ALREADY_EXISTS:
    case grpc::StatusCode::ABORTED:
      return 409;
    case grpc::StatusCode::FAILED_PRECONDITION:
      return 412;
    case grpc::StatusCode::RESOURCE_EXHAUSTED:
      return 429;
    case grpc::StatusCode::UNIMPLEMENTED:
      return 501;
    case grpc::StatusCode::UNAVAILABLE:
      return 503;
    case grpc::StatusCode::DEADLINE_EXCEEDED:
      return 504;
    case grpc::StatusCode::CANCELLED:
    case grpc::StatusCode::UNKNOWN:
    case grpc::StatusCode::DATA_LOSS:
    case grpc::StatusCode::INTERNAL:
    case grpc::StatusCode::DO_NOT_USE:
    default:
      return 500;
    }
  }

  // Stable, machine-readable name for a gRPC code (used in the JSON body).
  [[nodiscard]] inline std::string grpc_code_name(grpc::StatusCode code) {
    switch (code) {
    case grpc::StatusCode::OK:
      return "OK";
    case grpc::StatusCode::CANCELLED:
      return "CANCELLED";
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
    case grpc::StatusCode::UNAVAILABLE:
      return "UNAVAILABLE";
    case grpc::StatusCode::UNAUTHENTICATED:
      return "UNAUTHENTICATED";
    case grpc::StatusCode::INTERNAL:
    case grpc::StatusCode::UNKNOWN:
    case grpc::StatusCode::DATA_LOSS:
    case grpc::StatusCode::DO_NOT_USE:
    default:
      return "INTERNAL";
    }
  }

  [[nodiscard]] inline HttpError to_http_error(const grpc::Status& status) {
    const auto code = status.error_code();
    nlohmann::json body{
        {"code", grpc_code_name(code)},
        {"message", status.error_message()},
    };
    return HttpError{.status_code = http_status_for(code), .body = body.dump()};
  }

} // namespace fmgr::rest

#endif // FMGR_REST_RESTERRORTRANSLATION_H
