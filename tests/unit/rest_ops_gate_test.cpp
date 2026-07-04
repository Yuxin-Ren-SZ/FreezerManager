// SPDX-License-Identifier: AGPL-3.0-or-later

// Unit coverage for the operational-endpoint exposure gate (review C7): the
// pure decision behind /health readiness and /metrics when they are not public.

#include "rest/RestGateway.h"

#include <gtest/gtest.h>

#include <optional>
#include <string_view>

namespace {

  using fmgr::rest::OpsAuthorizer;
  using fmgr::rest::RestGateway;

  // A public endpoint always answers, regardless of bearer or authorizer.
  TEST(RestOpsGate, PublicEndpointAlwaysPermitted) {
    const OpsAuthorizer never = [](std::optional<std::string_view>) { return false; };
    EXPECT_TRUE(RestGateway::ops_endpoint_permitted(true, never, std::nullopt));
    EXPECT_TRUE(RestGateway::ops_endpoint_permitted(true, never, std::string_view("Bearer x")));
    EXPECT_TRUE(RestGateway::ops_endpoint_permitted(true, {}, std::nullopt));
  }

  // A private endpoint with no Authorization header is denied.
  TEST(RestOpsGate, PrivateEndpointDeniesMissingHeader) {
    const OpsAuthorizer accept_present = [](std::optional<std::string_view> header) {
      return header.has_value();
    };
    EXPECT_FALSE(RestGateway::ops_endpoint_permitted(false, accept_present, std::nullopt));
  }

  // A private endpoint honors the authorizer's verdict on the bearer.
  TEST(RestOpsGate, PrivateEndpointHonorsAuthorizer) {
    const OpsAuthorizer accept_good = [](std::optional<std::string_view> header) {
      return header.has_value() && *header == "Bearer good";
    };
    EXPECT_TRUE(
        RestGateway::ops_endpoint_permitted(false, accept_good, std::string_view("Bearer good")));
    EXPECT_FALSE(
        RestGateway::ops_endpoint_permitted(false, accept_good, std::string_view("Bearer bad")));
  }

  // A private endpoint with no authorizer installed fails closed.
  TEST(RestOpsGate, PrivateEndpointFailsClosedWithoutAuthorizer) {
    EXPECT_FALSE(RestGateway::ops_endpoint_permitted(false, {}, std::string_view("Bearer x")));
  }

} // namespace
