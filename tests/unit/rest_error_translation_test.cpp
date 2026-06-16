// SPDX-License-Identifier: AGPL-3.0-or-later

#include "rest/RestErrorTranslation.h"

#include <grpcpp/support/status.h>
#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

namespace fmgr::rest {
  namespace {

    TEST(RestErrorTranslationTest, MapsGrpcCodesToHttpStatus) {
      EXPECT_EQ(http_status_for(grpc::StatusCode::OK), 200);
      EXPECT_EQ(http_status_for(grpc::StatusCode::INVALID_ARGUMENT), 400);
      EXPECT_EQ(http_status_for(grpc::StatusCode::OUT_OF_RANGE), 400);
      EXPECT_EQ(http_status_for(grpc::StatusCode::UNAUTHENTICATED), 401);
      EXPECT_EQ(http_status_for(grpc::StatusCode::PERMISSION_DENIED), 403);
      EXPECT_EQ(http_status_for(grpc::StatusCode::NOT_FOUND), 404);
      EXPECT_EQ(http_status_for(grpc::StatusCode::ALREADY_EXISTS), 409);
      EXPECT_EQ(http_status_for(grpc::StatusCode::ABORTED), 409);
      EXPECT_EQ(http_status_for(grpc::StatusCode::FAILED_PRECONDITION), 412);
      EXPECT_EQ(http_status_for(grpc::StatusCode::RESOURCE_EXHAUSTED), 429);
      EXPECT_EQ(http_status_for(grpc::StatusCode::UNIMPLEMENTED), 501);
      EXPECT_EQ(http_status_for(grpc::StatusCode::UNAVAILABLE), 503);
      EXPECT_EQ(http_status_for(grpc::StatusCode::DEADLINE_EXCEEDED), 504);
    }

    TEST(RestErrorTranslationTest, UnknownAndInternalCodesMapTo500) {
      EXPECT_EQ(http_status_for(grpc::StatusCode::INTERNAL), 500);
      EXPECT_EQ(http_status_for(grpc::StatusCode::UNKNOWN), 500);
      EXPECT_EQ(http_status_for(grpc::StatusCode::DATA_LOSS), 500);
      EXPECT_EQ(http_status_for(grpc::StatusCode::CANCELLED), 500);
    }

    TEST(RestErrorTranslationTest, BodyCarriesCodeNameAndMessage) {
      const grpc::Status status{grpc::StatusCode::PERMISSION_DENIED, "no sample.read"};
      const auto err = to_http_error(status);

      EXPECT_EQ(err.status_code, 403);
      const auto body = nlohmann::json::parse(err.body);
      EXPECT_EQ(body.at("code"), "PERMISSION_DENIED");
      EXPECT_EQ(body.at("message"), "no sample.read");
    }

    TEST(RestErrorTranslationTest, OkStatusProduces200) {
      const auto err = to_http_error(grpc::Status::OK);
      EXPECT_EQ(err.status_code, 200);
      EXPECT_EQ(nlohmann::json::parse(err.body).at("code"), "OK");
    }

    // ---- Break-it: cover all remaining gRPC status codes ----

    TEST(RestErrorTranslationTest, AllStandardGrpcCodesHaveHttpMapping) {
      // Every standard gRPC code must produce a known HTTP status (≥200, ≤599).
      for (int i = 0; i <= 16; ++i) {
        const auto code = static_cast<grpc::StatusCode>(i);
        const auto status = http_status_for(code);
        EXPECT_GE(status, 200);
        EXPECT_LE(status, 599);
      }
    }

    TEST(RestErrorTranslationTest, EmptyErrorMessageMapsToEmptyJsonMessage) {
      const grpc::Status status{grpc::StatusCode::NOT_FOUND, ""};
      const auto err = to_http_error(status);
      const auto body = nlohmann::json::parse(err.body);
      EXPECT_EQ(body.at("code"), "NOT_FOUND");
      EXPECT_EQ(body.at("message"), "");
    }

    TEST(RestErrorTranslationTest, SpecialCharactersInErrorMessage) {
      const grpc::Status status{grpc::StatusCode::INVALID_ARGUMENT,
                                "field \"name\" contains <script> & 'quotes'"};
      const auto err = to_http_error(status);
      const auto body = nlohmann::json::parse(err.body);
      EXPECT_EQ(body.at("code"), "INVALID_ARGUMENT");
      // The message must survive round-tripping through JSON encoding.
      EXPECT_EQ(body.at("message"), "field \"name\" contains <script> & 'quotes'");
    }

    TEST(RestErrorTranslationTest, UnicodeInErrorMessage) {
      const grpc::Status status{grpc::StatusCode::FAILED_PRECONDITION, "输入值无效 🔥"};
      const auto err = to_http_error(status);
      const auto body = nlohmann::json::parse(err.body);
      EXPECT_EQ(body.at("code"), "FAILED_PRECONDITION");
      EXPECT_EQ(body.at("message"), "输入值无效 🔥");
    }

    TEST(RestErrorTranslationTest, VeryLongErrorMessageDoesNotCrash) {
      const std::string long_msg(10'000, 'x');
      const grpc::Status status{grpc::StatusCode::INTERNAL, long_msg};
      const auto err = to_http_error(status);
      EXPECT_EQ(err.status_code, 500);
      const auto body = nlohmann::json::parse(err.body);
      EXPECT_EQ(body.at("code"), "INTERNAL");
      EXPECT_EQ(body.at("message").get<std::string>().size(), 10'000U);
    }

    TEST(RestErrorTranslationTest, StatusOkWithMessageProduces200) {
      const auto err = to_http_error(grpc::Status{grpc::StatusCode::OK, "all good"});
      EXPECT_EQ(err.status_code, 200);
    }

    // ==== Aggressive: JSON injection, null bytes, boundary lengths ====

    TEST(RestErrorTranslationTest, JsonInjectionInMessageIsEscaped) {
      const grpc::Status status{grpc::StatusCode::INVALID_ARGUMENT,
                                R"({"fake":"json\"}, \"real\":\"leak\"})"};
      const auto err = to_http_error(status);
      // The body must be valid JSON; the message must be a single escaped string,
      // not injected JSON structure.
      const auto body = nlohmann::json::parse(err.body);
      EXPECT_EQ(body.at("code"), "INVALID_ARGUMENT");
      EXPECT_EQ(body.size(), 2U); // code, message
    }

    TEST(RestErrorTranslationTest, NullByteInErrorMessage) {
      const std::string msg("before\0after", 12);
      const grpc::Status status{grpc::StatusCode::INTERNAL, msg};
      const auto err = to_http_error(status);
      EXPECT_EQ(err.status_code, 500);
      const auto body = nlohmann::json::parse(err.body);
      EXPECT_EQ(body.at("code"), "INTERNAL");
    }

    TEST(RestErrorTranslationTest, TabAndNewlineInErrorMessage) {
      const grpc::Status status{grpc::StatusCode::INTERNAL, "line1\nline2\tindented"};
      const auto err = to_http_error(status);
      EXPECT_EQ(err.status_code, 500);
      const auto body = nlohmann::json::parse(err.body);
      EXPECT_EQ(body.at("code"), "INTERNAL");
    }

    TEST(RestErrorTranslationTest, StatusCodeOutOfEnumRange) {
      // gRPC defines codes 0–16. Values outside that range are implementation-defined.
      const auto status = http_status_for(static_cast<grpc::StatusCode>(99));
      EXPECT_GE(status, 200);
      EXPECT_LE(status, 599);
    }

  } // namespace
} // namespace fmgr::rest
