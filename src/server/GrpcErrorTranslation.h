// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FMGR_SERVER_GRPCERRORTRANSLATION_H
#define FMGR_SERVER_GRPCERRORTRANSLATION_H

#include "auth/AuthTypes.h"
#include "storage/IStorageBackend.h"

#include <grpcpp/grpcpp.h>

#include <exception>
#include <string>

namespace fmgr::server {

  [[nodiscard]] inline grpc::Status to_grpc_status(const auth::InvalidCredentials& e) {
    return {grpc::StatusCode::UNAUTHENTICATED, e.what()};
  }

  [[nodiscard]] inline grpc::Status to_grpc_status(const auth::TokenExpired& e) {
    return {grpc::StatusCode::UNAUTHENTICATED, e.what()};
  }

  [[nodiscard]] inline grpc::Status to_grpc_status(const auth::TokenRevoked& e) {
    return {grpc::StatusCode::UNAUTHENTICATED, e.what()};
  }

  [[nodiscard]] inline grpc::Status to_grpc_status(const auth::MfaRequired& e) {
    return {grpc::StatusCode::UNAUTHENTICATED, std::string("mfa_required: ") + e.what()};
  }

  [[nodiscard]] inline grpc::Status to_grpc_status(const auth::AccountLocked& e) {
    return {grpc::StatusCode::PERMISSION_DENIED, e.what()};
  }

  [[nodiscard]] inline grpc::Status to_grpc_status(const auth::PermissionDenied& e) {
    return {grpc::StatusCode::PERMISSION_DENIED, e.what()};
  }

  [[nodiscard]] inline grpc::Status to_grpc_status(const storage::NotFound& e) {
    return {grpc::StatusCode::NOT_FOUND, e.what()};
  }

  [[nodiscard]] inline grpc::Status to_grpc_status(const storage::UniqueViolation& e) {
    return {grpc::StatusCode::ALREADY_EXISTS, e.what()};
  }

  [[nodiscard]] inline grpc::Status to_grpc_status(const storage::ConstraintViolation& e) {
    return {grpc::StatusCode::INVALID_ARGUMENT, e.what()};
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
    } catch (const std::exception& e) {
      return {grpc::StatusCode::INTERNAL, e.what()};
    } catch (...) {
      return {grpc::StatusCode::INTERNAL, "unknown internal error"};
    }
  }

  // Extract "Bearer <token>" from gRPC request metadata.
  // Throws auth::InvalidCredentials if header is missing or malformed.
  [[nodiscard]] inline std::string extract_bearer(const grpc::ServerContext& ctx) {
    const auto& metadata = ctx.client_metadata();
    const auto it = metadata.find("authorization");
    if (it == metadata.end()) {
      throw auth::InvalidCredentials("missing Authorization header");
    }
    const std::string_view value(it->second.data(), it->second.size());
    constexpr std::string_view prefix = "Bearer ";
    if (!value.starts_with(prefix)) {
      throw auth::InvalidCredentials("Authorization header must be 'Bearer <token>'");
    }
    return std::string(value.substr(prefix.size()));
  }

} // namespace fmgr::server

#endif // FMGR_SERVER_GRPCERRORTRANSLATION_H
