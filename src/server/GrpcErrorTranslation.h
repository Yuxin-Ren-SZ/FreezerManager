// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FMGR_SERVER_GRPCERRORTRANSLATION_H
#define FMGR_SERVER_GRPCERRORTRANSLATION_H

#include "auth/AuthTypes.h"
#include "obs/Log.h"
#include "rpc/AuthMiddleware.h"
#include "rpc/ErrorCodes.h"
#include "rpc/RpcMethodTracker.h"
#include "storage/IStorageBackend.h"

#include <fmt/format.h>
#include <grpcpp/grpcpp.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <exception>
#include <optional>
#include <string>
#include <string_view>

namespace fmgr::server {

  // ---- Internal-error masking (security audit C-11: info leak) ----
  //
  // When enabled, INTERNAL failures return a generic message to the client and
  // the real detail is only logged server-side. Disabled builds surface the
  // detail in the status message for developer convenience. Default-on is the
  // safe choice: a deployment that never toggles it cannot leak. The server sets
  // this from FreezerServerOptions::mask_internal_errors at build().
  inline std::atomic<bool>& internal_error_masking() {
    static std::atomic<bool> s_mask{true};
    return s_mask;
  }

  inline void set_mask_internal_errors(bool mask) {
    internal_error_masking().store(mask, std::memory_order_release);
  }

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
    // The marker is a named constant shared with the REST gateway, which turns it
    // into the machine-readable envelope code `MFA_REQUIRED` (#140) — a client
    // that has to resume the TOTP step must not be keying on a sentence.
    return {grpc::StatusCode::UNAUTHENTICATED,
            std::string(rpc::k_mfa_required_marker) + error.what()};
  }

  [[nodiscard]] inline grpc::Status to_grpc_status(const auth::AccountLocked& error) {
    return {grpc::StatusCode::PERMISSION_DENIED, error.what()};
  }

  [[nodiscard]] inline grpc::Status to_grpc_status(const auth::PermissionDenied& error) {
    return {grpc::StatusCode::PERMISSION_DENIED, error.what()};
  }

  [[nodiscard]] inline grpc::Status to_grpc_status(const auth::RateLimited& error) {
    return {grpc::StatusCode::RESOURCE_EXHAUSTED, error.what()};
  }

  // ---- Engine text is logged, not sent (#123) ----
  //
  // A storage exception's what() is either a sentence we wrote for the caller or,
  // when the engine refused the row, the class's client-safe default — the engine
  // text itself rides in detail() (storage::BackendText). These failures are the
  // ones where that text names a constraint, so it is worth keeping server-side:
  // it is the only record of *which* index refused the write.
  //
  // Only the first line is logged. PostgreSQL's text is PQresultErrorMessage(),
  // multi-line, and its DETAIL line carries the values that collided — which can
  // be PHI, and PHI never goes in a log (AGENTS.md §5). The first line names the
  // constraint without quoting a row.
  inline void log_refused_write(const storage::BackendError& error) {
    if (error.detail().empty()) {
      return;
    }
    const auto newline = error.detail().find('\n');
    obs::log_lifecycle(
        obs::Level::Info,
        fmt::format("storage refused a write: {}", error.detail().substr(0, newline)),
        "storage.write_refused");
  }

  [[nodiscard]] inline grpc::Status to_grpc_status(const storage::NotFound& error) {
    return {grpc::StatusCode::NOT_FOUND, error.what()};
  }

  [[nodiscard]] inline grpc::Status to_grpc_status(const storage::UniqueViolation& error) {
    log_refused_write(error);
    return {grpc::StatusCode::ALREADY_EXISTS, error.what()};
  }

  [[nodiscard]] inline grpc::Status to_grpc_status(const storage::ConstraintViolation& error) {
    log_refused_write(error);
    return {grpc::StatusCode::INVALID_ARGUMENT, error.what()};
  }

  [[nodiscard]] inline grpc::Status to_grpc_status(const storage::ForeignKeyViolation& error) {
    log_refused_write(error);
    return {grpc::StatusCode::FAILED_PRECONDITION, error.what()};
  }

  // A serialization conflict (Postgres 40001) is transient — the client may retry
  // the whole RPC. ABORTED is the gRPC-canonical signal for that.
  [[nodiscard]] inline grpc::Status to_grpc_status(const storage::SerializationFailure& error) {
    log_refused_write(error);
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
    } catch (const auth::RateLimited& e) {
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
    } catch (const rpc::RpcRegistryMismatch& e) {
      // The permission a handler enforces disagrees with the one its RPC
      // registered (#60) — a deployment defect, not a caller error. It gets its
      // own event code so a production occurrence is one grep away
      // ("grpc.registry_mismatch") instead of hiding among generic internal
      // errors; the detail is masked on the wire exactly like every other
      // internal failure.
      obs::log_lifecycle(obs::Level::Error,
                         fmt::format("grpc: RPC registry mismatch: {}", e.what()),
                         "grpc.registry_mismatch");
      if (internal_error_masking().load(std::memory_order_acquire)) {
        return {grpc::StatusCode::INTERNAL, "internal server error"};
      }
      return {grpc::StatusCode::INTERNAL, fmt::format("internal server error: {}", e.what())};
    } catch (const std::exception& e) {
      // Do not leak internal detail (DB messages carry table/column names) to the
      // client when masking is on. Log the real error server-side; return a
      // generic status. spdlog's async sink keeps this off the RPC thread's hot
      // path, unlike unbuffered std::cerr which serializes a write() syscall per
      // error (audit H-3). Masking is toggled off in debug builds so developers
      // see the real detail on the wire (audit C-11).
      obs::log_lifecycle(obs::Level::Error,
                         fmt::format("grpc: unhandled internal error: {}", e.what()),
                         "grpc.internal_error");
      if (internal_error_masking().load(std::memory_order_acquire)) {
        return {grpc::StatusCode::INTERNAL, "internal server error"};
      }
      return {grpc::StatusCode::INTERNAL, fmt::format("internal server error: {}", e.what())};
    } catch (...) {
      obs::log_lifecycle(obs::Level::Error, "grpc: unhandled non-std exception",
                         "grpc.internal_error");
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

  // The "Bearer <token>" value of the Authorization header. Throws
  // auth::InvalidCredentials when the header is missing or malformed.
  [[nodiscard]] inline std::string bearer_token(const grpc::ServerContext& ctx) {
    const auto& metadata = ctx.client_metadata();
    const auto it = metadata.find("authorization");
    return it == metadata.end()
               ? parse_bearer(std::nullopt)
               : parse_bearer(std::string_view(it->second.data(), it->second.size()));
  }

  // The full name of the RPC being served, for AuthMiddleware's registry checks.
  // Reads no credential and throws nothing: a handler whose RPC declares it needs
  // no credential (#119, Login) must not be refused for an Authorization header it
  // was never going to read, so an absent or malformed one is not an error here.
  [[nodiscard]] inline std::string rpc_method_name(const grpc::ServerContext& ctx) {
    return rpc::RpcMethodTracker::lookup(static_cast<const grpc::ServerContextBase*>(&ctx));
  }

  // Extract "Bearer <token>" from gRPC request metadata, together with the full
  // name of the RPC being served (rpc::RpcCall).
  //
  // Handlers hand the result straight to AuthMiddleware::authorize(), which uses
  // the method name to check the permission the handler enforces against the
  // permission its RPC registered (#60). Filling it here — from the same
  // ServerContext the handler already passes — is what keeps that check free of
  // per-handler plumbing. The name comes from rpc::RpcMethodTracker, which the
  // server's per-RPC interceptor fills; gRPC's ServerContext has no accessor for
  // it in this version.
  //
  // Throws auth::InvalidCredentials if header is missing or malformed.
  [[nodiscard]] inline rpc::RpcCall extract_bearer(const grpc::ServerContext& ctx) {
    return rpc::RpcCall{.bearer_token = bearer_token(ctx), .method = rpc_method_name(ctx)};
  }

} // namespace fmgr::server

#endif // FMGR_SERVER_GRPCERRORTRANSLATION_H
