// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/json_helpers.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace fmgr::core::json_helpers {
  namespace {

    // ---- opt_to_json ----

    TEST(JsonHelpers, OptToJsonNulloptYieldsNull) {
      const std::optional<int> empty;
      const auto json = opt_to_json(empty);
      EXPECT_TRUE(json.is_null());
    }

    TEST(JsonHelpers, OptToJsonValueYieldsWrappedValue) {
      const std::optional<int> value{42};
      const auto json = opt_to_json(value);
      ASSERT_TRUE(json.is_number_integer());
      EXPECT_EQ(json.get<int>(), 42);
    }

    TEST(JsonHelpers, OptToJsonStringPreservesType) {
      const std::optional<std::string> value{"hello"};
      const auto json = opt_to_json(value);
      ASSERT_TRUE(json.is_string());
      EXPECT_EQ(json.get<std::string>(), "hello");
    }

    TEST(JsonHelpers, OptToJsonEmptyStringPreservesType) {
      const std::optional<std::string> value{""};
      const auto json = opt_to_json(value);
      ASSERT_TRUE(json.is_string());
      EXPECT_EQ(json.get<std::string>(), "");
    }

    TEST(JsonHelpers, OptToJsonBoolPreservesType) {
      const std::optional<bool> value{true};
      const auto json = opt_to_json(value);
      ASSERT_TRUE(json.is_boolean());
      EXPECT_EQ(json.get<bool>(), true);
    }

    // ---- opt_from_json ----

    TEST(JsonHelpers, OptFromJsonNullYieldsNullopt) {
      const nlohmann::json json = nullptr;
      const auto result = opt_from_json<int>(json);
      EXPECT_FALSE(result.has_value());
    }

    TEST(JsonHelpers, OptFromJsonValueYieldsOptional) {
      const nlohmann::json json = 42;
      const auto result = opt_from_json<int>(json);
      ASSERT_TRUE(result.has_value());
      EXPECT_EQ(result.value(), 42);
    }

    TEST(JsonHelpers, OptFromJsonStringRoundTrips) {
      const std::optional<std::string> original = "round-trip";
      const auto json = opt_to_json(original);
      const auto restored = opt_from_json<std::string>(json);
      ASSERT_TRUE(restored.has_value());
      EXPECT_EQ(restored.value(), "round-trip");
    }

    TEST(JsonHelpers, OptFromJsonIntRoundTrips) {
      const std::optional<int> original = -7;
      const auto json = opt_to_json(original);
      const auto restored = opt_from_json<int>(json);
      ASSERT_TRUE(restored.has_value());
      EXPECT_EQ(restored.value(), -7);
    }

    // ---- composition ----

    TEST(JsonHelpers, RoundTripDoublePreservesApproximateValue) {
      const std::optional<double> original = 3.14;
      const auto json = opt_to_json(original);
      const auto restored = opt_from_json<double>(json);
      ASSERT_TRUE(restored.has_value());
      EXPECT_DOUBLE_EQ(restored.value(), 3.14);
    }

    TEST(JsonHelpers, RoundTripNullPreservesNullopt) {
      const std::optional<int> original;
      const auto json = opt_to_json(original);
      const auto restored = opt_from_json<int>(json);
      EXPECT_FALSE(restored.has_value());
    }

  } // namespace
} // namespace fmgr::core::json_helpers
