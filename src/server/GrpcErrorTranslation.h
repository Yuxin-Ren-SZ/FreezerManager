// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FMGR_SERVER_GRPCERRORTRANSLATION_H
#define FMGR_SERVER_GRPCERRORTRANSLATION_H

#include "auth/AuthTypes.h"
#include "storage/IStorageBackend.h"

#include <grpcpp/grpcpp.h>

#include <exception>
#include <iostream>
#include <string>

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
    } catch (const std::exception& e) {
      // Do not leak internal detail (DB messages carry table/column names) to the
      // client. Log the real error server-side; return a generic status.
      std::cerr << "grpc: unhandled internal error: " << e.what() << '\n';
      return {grpc::StatusCode::INTERNAL, "internal server error"};
    } catch (...) {
      std::cerr << "grpc: unhandled non-std exception\n";
      return {grpc::StatusCode::INTERNAL, "internal server error"};
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
