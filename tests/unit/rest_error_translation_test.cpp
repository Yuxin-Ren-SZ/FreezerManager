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

  } // namespace
} // namespace fmgr::rest
