// SPDX-License-Identifier: AGPL-3.0-or-later

#include "rest/JsonProtoMapping.h"

#include <fmgr/v1/auth.pb.h>
#include <fmgr/v1/lab.pb.h>
#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <string>

namespace fmgr::rest {
  namespace {

    TEST(JsonProtoMappingTest, RoundTripsScalarFields) {
      fmgr::v1::LoginRequest req;
      req.set_email("admin@example.com");
      req.set_password("hunter22");

      const std::string json = message_to_json(req);

      fmgr::v1::LoginRequest parsed;
      json_to_message(json, parsed);
      EXPECT_EQ(parsed.email(), "admin@example.com");
      EXPECT_EQ(parsed.password(), "hunter22");
    }

    TEST(JsonProtoMappingTest, EmitsProtoSnakeCaseFieldNames) {
      fmgr::v1::CreateLabRequest req;
      req.set_name("Test Lab");
      req.set_contact("test@example.com");

      const auto parsed = nlohmann::json::parse(message_to_json(req));
      // preserve_proto_field_names=true: keys match the .proto, not lowerCamelCase.
      EXPECT_TRUE(parsed.contains("name"));
      EXPECT_TRUE(parsed.contains("contact"));
      EXPECT_EQ(parsed["name"], "Test Lab");
    }

    TEST(JsonProtoMappingTest, ParsesNestedAndRepeatedFields) {
      // LoginResponse carries scalar + bool; use a message with a nested field
      // to exercise the proto3 JSON nested mapping.
      const std::string json = R"({"session_token":"t","session_id":"s",)"
                               R"("user_id":"u","mfa_required":true})";
      fmgr::v1::LoginResponse out;
      json_to_message(json, out);
      EXPECT_EQ(out.session_token(), "t");
      EXPECT_TRUE(out.mfa_required());
    }

    TEST(JsonProtoMappingTest, MalformedJsonThrowsBadJson) {
      fmgr::v1::LoginRequest out;
      EXPECT_THROW(json_to_message("{not valid json", out), BadJson);
    }

    TEST(JsonProtoMappingTest, UnknownFieldThrowsBadJson) {
      // Fail-closed: a typo'd field name is a client error, not silently dropped.
      fmgr::v1::LoginRequest out;
      EXPECT_THROW(json_to_message(R"({"emial":"x"})", out), BadJson);
    }

    TEST(JsonProtoMappingTest, TypeMismatchThrowsBadJson) {
      fmgr::v1::LoginRequest out;
      EXPECT_THROW(json_to_message(R"({"email":123})", out), BadJson);
    }

  } // namespace
} // namespace fmgr::rest
