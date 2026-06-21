// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FMGR_SERVER_GRPCERRORTRANSLATION_H
#define FMGR_SERVER_GRPCERRORTRANSLATION_H

#include "auth/AuthTypes.h"
#include "storage/IStorageBackend.h"

#include <grpcpp/grpcpp.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <exception>
#include <optional>
#include <string>
#include <string_view>

namespace fmgr::server {

  [[nodiscard]] inline grpc::Status to_grpc_status(const auth::InvalidCredentials& error) {
    return {grpc::StatusCode::UNAUTHENTICATED, error.what()};
  }

  [[nodiscard]] inline grpc::Status to_grpc_status(const auth::TokenExpired& error) {
    return {grpc::StatusCode::UNAUTHENTICATED, error.what()};
  }

  [[nodiscard]] inline grpc::Status to_grpc_status(const auth::TokenRevoked& error) {
    return {grpc::StatusCode::UNAUTHENTICATED, error.what()};
  }

  [[nodiscard]] inline grpc::Status to_grpc_status(const auth::MfaRequired& error) {
    return {grpc::StatusCode::UNAUTHENTICATED, std::string("mfa_required: ") + error.what()};
  }

  [[nodiscard]] inline grpc::Status to_grpc_status(const auth::AccountLocked& error) {
    return {grpc::StatusCode::PERMISSION_DENIED, error.what()};
  }

  [[nodiscard]] inline grpc::Status to_grpc_status(const auth::PermissionDenied& error) {
    return {grpc::StatusCode::PERMISSION_DENIED, error.what()};
  }

  [[nodiscard]] inline grpc::Status to_grpc_status(const storage::NotFound& error) {
    return {grpc::StatusCode::NOT_FOUND, error.what()};
  }

  [[nodiscard]] inline grpc::Status to_grpc_status(const storage::UniqueViolation& error) {
    return {grpc::StatusCode::ALREADY_EXISTS, error.what()};
  }

  [[nodiscard]] inline grpc::Status to_grpc_status(const storage::ConstraintViolation& error) {
    return {grpc::StatusCode::INVALID_ARGUMENT, error.what()};
  }

  [[nodiscard]] inline grpc::Status to_grpc_status(const storage::ForeignKeyViolation& error) {
    return {grpc::StatusCode::FAILED_PRECONDITION, error.what()};
  }

  // A serialization conflict (Postgres 40001) is transient — the client may retry
  // the whole RPC. ABORTED is the gRPC-canonical signal for that.
  [[nodiscard]] inline grpc::Status to_grpc_status(const storage::SerializationFailure& /*error*/) {
    return {grpc::StatusCode::ABORTED, "transaction conflict; retry the request"};
  }

  [[nodiscard]] inline grpc::Status to_grpc_status(const storage::Unavailable& /*error*/) {
    return {grpc::StatusCode::UNAVAILABLE, "storage backend unavailable"};
  }

  // Translate any exception to a gRPC status. Must be called inside a catch block.
  [[nodiscard]] inline grpc::Status current_exception_to_grpc_status() {
    try {
      throw;
    } catch (const auth::MfaRequired& e) {
      return to_grpc_status(e);
    } catch (const auth::PermissionDenied& e) {
      return to_grpc_status(e);
    } catch (const auth::AccountLocked& e) {
      return to_grpc_status(e);
    } catch (const auth::TokenExpired& e) {
      return to_grpc_status(e);
    } catch (const auth::TokenRevoked& e) {
      return to_grpc_status(e);
    } catch (const auth::InvalidCredentials& e) {
      return to_grpc_status(e);
    } catch (const storage::NotFound& e) {
      return to_grpc_status(e);
    } catch (const storage::UniqueViolation& e) {
      return to_grpc_status(e);
    } catch (const storage::ConstraintViolation& e) {
      return to_grpc_status(e);
    } catch (const storage::ForeignKeyViolation& e) {
      return to_grpc_status(e);
    } catch (const storage::SerializationFailure& e) {
      return to_grpc_status(e);
    } catch (const storage::Unavailable& e) {
      return to_grpc_status(e);
    } catch (const nlohmann::json::parse_error&) {
      // Client supplied malformed JSON (e.g. custom_fields_json / settings_json).
      // That is a bad argument, not an internal fault (review N-1). The detail is
      // not echoed back: a parse-error snippet could carry client PHI.
      return {grpc::StatusCode::INVALID_ARGUMENT, "request contained malformed JSON"};
    } catch (const std::exception& e) {
      // Do not leak internal detail (DB messages carry table/column names) to the
      // client. Log the real error server-side; return a generic status. spdlog's
      // async sink keeps this off the RPC thread's hot path, unlike unbuffered
      // std::cerr which serializes a write() syscall per error (audit H-3).
      spdlog::error("grpc: unhandled internal error: {}", e.what());
      return {grpc::StatusCode::INTERNAL, "internal server error"};
    } catch (...) {
      spdlog::error("grpc: unhandled non-std exception");
      return {grpc::StatusCode::INTERNAL, "internal server error"};
    }
  }

  // Parse a "Bearer <token>" Authorization header value. `header` is nullopt when
  // the header is absent. Throws auth::InvalidCredentials when the header is
  // missing or not of the form "Bearer <token>". Factored out of extract_bearer
  // so the parsing is unit-testable without a live grpc::ServerContext, whose
  // server-side client_metadata() cannot be populated in-process.
  [[nodiscard]] inline std::string parse_bearer(std::optional<std::string_view> header) {
    if (!header.has_value()) {
      throw auth::InvalidCredentials("missing Authorization header");
    }
    constexpr std::string_view prefix = "Bearer ";
    if (!header->starts_with(prefix)) {
      throw auth::InvalidCredentials("Authorization header must be 'Bearer <token>'");
    }
    return std::string(header->substr(prefix.size()));
  }

  // Extract "Bearer <token>" from gRPC request metadata.
  // Throws auth::InvalidCredentials if header is missing or malformed.
  [[nodiscard]] inline std::string extract_bearer(const grpc::ServerContext& ctx) {
    const auto& metadata = ctx.client_metadata();
    const auto it = metadata.find("authorization");
    if (it == metadata.end()) {
      return parse_bearer(std::nullopt);
    }
    return parse_bearer(std::string_view(it->second.data(), it->second.size()));
  }

} // namespace fmgr::server

#endif // FMGR_SERVER_GRPCERRORTRANSLATION_H
