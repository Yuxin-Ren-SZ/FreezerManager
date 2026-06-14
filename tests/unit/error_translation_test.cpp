// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Exhaustive tests for every BackendError + AuthError → gRPC status translation.
// Also covers catch-all scenarios: unknown std::exception, non-std throwable,
// and the bearer-token extraction failures.

#include "auth/AuthTypes.h"
#include "server/GrpcErrorTranslation.h"
#include "storage/IStorageBackend.h"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fmgr::server {
  namespace {

    // ---- AuthError translations ----

    TEST(ErrorTranslation, InvalidCredentialsMapsToUnauthenticated) {
      const auth::InvalidCredentials error("wrong password");
      const auto status = to_grpc_status(error);
      EXPECT_EQ(status.error_code(), grpc::StatusCode::UNAUTHENTICATED);
      EXPECT_STREQ(status.error_message().c_str(), "wrong password");
    }

    TEST(ErrorTranslation, TokenExpiredMapsToUnauthenticated) {
      const auth::TokenExpired error("token expired at epoch");
      const auto status = to_grpc_status(error);
      EXPECT_EQ(status.error_code(), grpc::StatusCode::UNAUTHENTICATED);
      EXPECT_STREQ(status.error_message().c_str(), "token expired at epoch");
    }

    TEST(ErrorTranslation, TokenRevokedMapsToUnauthenticated) {
      const auth::TokenRevoked error("session was revoked");
      const auto status = to_grpc_status(error);
      EXPECT_EQ(status.error_code(), grpc::StatusCode::UNAUTHENTICATED);
      EXPECT_STREQ(status.error_message().c_str(), "session was revoked");
    }

    TEST(ErrorTranslation, MfaRequiredMapsToUnauthenticatedWithPrefix) {
      const auth::MfaRequired error("TOTP not yet verified");
      const auto status = to_grpc_status(error);
      EXPECT_EQ(status.error_code(), grpc::StatusCode::UNAUTHENTICATED);
      EXPECT_NE(status.error_message().find("mfa_required:"), std::string::npos);
      EXPECT_NE(status.error_message().find("TOTP not yet verified"), std::string::npos);
    }

    TEST(ErrorTranslation, AccountLockedMapsToPermissionDenied) {
      const auth::AccountLocked error(core::Timestamp::from_unix_micros(99999));
      const auto status = to_grpc_status(error);
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
      EXPECT_NE(status.error_message().find("99999"), std::string::npos);
    }

    TEST(ErrorTranslation, PermissionDeniedMapsToPermissionDenied) {
      const auth::PermissionDenied error("missing sample:write");
      const auto status = to_grpc_status(error);
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
      EXPECT_STREQ(status.error_message().c_str(), "missing sample:write");
    }

    // ---- BackendError translations ----

    TEST(ErrorTranslation, NotFoundMapsToNotFound) {
      const storage::NotFound error("sample not found");
      const auto status = to_grpc_status(error);
      EXPECT_EQ(status.error_code(), grpc::StatusCode::NOT_FOUND);
      EXPECT_STREQ(status.error_message().c_str(), "sample not found");
    }

    TEST(ErrorTranslation, UniqueViolationMapsToAlreadyExists) {
      const storage::UniqueViolation error("duplicate sample barcode");
      const auto status = to_grpc_status(error);
      EXPECT_EQ(status.error_code(), grpc::StatusCode::ALREADY_EXISTS);
      EXPECT_STREQ(status.error_message().c_str(), "duplicate sample barcode");
    }

    TEST(ErrorTranslation, ConstraintViolationMapsToInvalidArgument) {
      const storage::ConstraintViolation error("name must not be empty");
      const auto status = to_grpc_status(error);
      EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
      EXPECT_STREQ(status.error_message().c_str(), "name must not be empty");
    }

    TEST(ErrorTranslation, ForeignKeyViolationMapsToFailedPrecondition) {
      const storage::ForeignKeyViolation error("referenced box does not exist");
      const auto status = to_grpc_status(error);
      EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
      EXPECT_STREQ(status.error_message().c_str(), "referenced box does not exist");
    }

    TEST(ErrorTranslation, SerializationFailureMapsToAborted) {
      const storage::SerializationFailure error("could not serialize access");
      const auto status = to_grpc_status(error);
      // SerializationFailure discards the original message and returns a fixed retry hint.
      EXPECT_EQ(status.error_code(), grpc::StatusCode::ABORTED);
      EXPECT_NE(status.error_message().find("retry"), std::string::npos);
      // Must NOT leak the internal detail.
      EXPECT_EQ(status.error_message().find("serialize"), std::string::npos);
    }

    TEST(ErrorTranslation, UnavailableMapsToUnavailable) {
      const storage::Unavailable error("database is locked");
      const auto status = to_grpc_status(error);
      // Unavailable discards the original message so client doesn't learn backend internals.
      EXPECT_EQ(status.error_code(), grpc::StatusCode::UNAVAILABLE);
      EXPECT_NE(status.error_message().find("storage"), std::string::npos);
      // Must NOT leak internal detail.
      EXPECT_EQ(status.error_message().find("lock"), std::string::npos);
    }

    // ---- current_exception_to_grpc_status (catch-block dispatch) ----

    TEST(ErrorTranslation, CurrentExceptionCatchesAuthErrors) {
      // Verify the catch-block chain dispatches each AuthError subclass correctly
      // by throwing, catching, and checking the gRPC code.
      const auto check_throw = [](auto&& factory) {
        try {
          throw factory();
        } catch (...) {
          return current_exception_to_grpc_status();
        }
      };

      EXPECT_EQ(check_throw([] { return auth::InvalidCredentials("x"); }).error_code(),
                grpc::StatusCode::UNAUTHENTICATED);
      EXPECT_EQ(check_throw([] { return auth::TokenExpired("x"); }).error_code(),
                grpc::StatusCode::UNAUTHENTICATED);
      EXPECT_EQ(check_throw([] { return auth::TokenRevoked("x"); }).error_code(),
                grpc::StatusCode::UNAUTHENTICATED);
      EXPECT_EQ(check_throw([] { return auth::MfaRequired("x"); }).error_code(),
                grpc::StatusCode::UNAUTHENTICATED);
      EXPECT_EQ(
          check_throw([] { return auth::AccountLocked(core::Timestamp::from_unix_micros(1)); })
              .error_code(),
          grpc::StatusCode::PERMISSION_DENIED);
      EXPECT_EQ(check_throw([] { return auth::PermissionDenied("x"); }).error_code(),
                grpc::StatusCode::PERMISSION_DENIED);
    }

    TEST(ErrorTranslation, CurrentExceptionCatchesBackendErrors) {
      const auto check_throw = [](auto&& factory) {
        try {
          throw factory();
        } catch (...) {
          return current_exception_to_grpc_status();
        }
      };

      EXPECT_EQ(check_throw([] { return storage::NotFound("x"); }).error_code(),
                grpc::StatusCode::NOT_FOUND);
      EXPECT_EQ(check_throw([] { return storage::UniqueViolation("x"); }).error_code(),
                grpc::StatusCode::ALREADY_EXISTS);
      EXPECT_EQ(check_throw([] { return storage::ConstraintViolation("x"); }).error_code(),
                grpc::StatusCode::INVALID_ARGUMENT);
      EXPECT_EQ(check_throw([] { return storage::ForeignKeyViolation("x"); }).error_code(),
                grpc::StatusCode::FAILED_PRECONDITION);
      EXPECT_EQ(check_throw([] { return storage::SerializationFailure("x"); }).error_code(),
                grpc::StatusCode::ABORTED);
      EXPECT_EQ(check_throw([] { return storage::Unavailable("x"); }).error_code(),
                grpc::StatusCode::UNAVAILABLE);
    }

    TEST(ErrorTranslation, CurrentExceptionCatchesBaseBackendError) {
      // BackendError base (MigrationFailure, UnsupportedOperation) is caught as
      // std::exception → INTERNAL since there's no specific catch for it.
      try {
        throw storage::MigrationFailure("schema version mismatch");
      } catch (...) {
        const auto status = current_exception_to_grpc_status();
        EXPECT_EQ(status.error_code(), grpc::StatusCode::INTERNAL);
        EXPECT_STREQ(status.error_message().c_str(), "internal server error");
        // Must NOT leak migration detail.
        EXPECT_EQ(status.error_message().find("schema"), std::string::npos);
      }
    }

    TEST(ErrorTranslation, CurrentExceptionOnUnknownStdExceptionMapsToInternal) {
      try {
        throw std::runtime_error("table=users column=ssn detail=leaked");
      } catch (...) {
        const auto status = current_exception_to_grpc_status();
        EXPECT_EQ(status.error_code(), grpc::StatusCode::INTERNAL);
        EXPECT_STREQ(status.error_message().c_str(), "internal server error");
        // Security critical — internal detail must NOT leak to the client.
        EXPECT_EQ(status.error_message().find("ssn"), std::string::npos);
        EXPECT_EQ(status.error_message().find("users"), std::string::npos);
        EXPECT_EQ(status.error_message().find("leaked"), std::string::npos);
      }
    }

    TEST(ErrorTranslation, CurrentExceptionOnUnknownStdLogicErrorMapsToInternal) {
      try {
        throw std::logic_error("assertion failed: invariant violated");
      } catch (...) {
        const auto status = current_exception_to_grpc_status();
        EXPECT_EQ(status.error_code(), grpc::StatusCode::INTERNAL);
        EXPECT_STREQ(status.error_message().c_str(), "internal server error");
        EXPECT_EQ(status.error_message().find("invariant"), std::string::npos);
      }
    }

    TEST(ErrorTranslation, CurrentExceptionOnNonStdThrowableMapsToInternal) {
      // Simulate a non-std throwable (e.g. a raw string thrown by a C library).
      struct NonStdThrowable {};
      try {
        throw NonStdThrowable{};
      } catch (...) {
        const auto status = current_exception_to_grpc_status();
        EXPECT_EQ(status.error_code(), grpc::StatusCode::INTERNAL);
        EXPECT_STREQ(status.error_message().c_str(), "internal server error");
      }
    }

    TEST(ErrorTranslation, AuthErrorsAreCaughtBeforeBackendErrors) {
      // AuthError is-a std::runtime_error. BackendError is-a std::runtime_error.
      // The catch blocks must check AuthErrors first, otherwise an AuthError
      // would be caught by the generic std::exception handler and surface as
      // INTERNAL instead of the specific gRPC code.
      try {
        throw auth::InvalidCredentials("bad password");
      } catch (...) {
        const auto status = current_exception_to_grpc_status();
        EXPECT_EQ(status.error_code(), grpc::StatusCode::UNAUTHENTICATED);
        EXPECT_NE(status.error_code(), grpc::StatusCode::INTERNAL);
      }
    }

    // ---- extract_bearer ----

    TEST(ErrorTranslation, ExtractBearerMissingHeaderThrows) {
      grpc::ServerContext ctx;
      EXPECT_THROW((void)extract_bearer(ctx), auth::InvalidCredentials);
    }

    TEST(ErrorTranslation, ExtractBearerMissingPrefixThrows) {
      grpc::ServerContext ctx;
      ctx.AddMetadata("authorization", "token-without-bearer-prefix");
      EXPECT_THROW((void)extract_bearer(ctx), auth::InvalidCredentials);
    }

    TEST(ErrorTranslation, ExtractBearerValidTokenReturnsTrimmed) {
      grpc::ServerContext ctx;
      ctx.AddMetadata("authorization", "Bearer my-secret-session-token");
      const auto token = extract_bearer(ctx);
      EXPECT_EQ(token, "my-secret-session-token");
    }

    TEST(ErrorTranslation, ExtractBearerEmptyTokenAfterBearerReturnsEmpty) {
      grpc::ServerContext ctx;
      ctx.AddMetadata("authorization", "Bearer ");
      const auto token = extract_bearer(ctx);
      EXPECT_TRUE(token.empty());
    }

  } // namespace
} // namespace fmgr::server
