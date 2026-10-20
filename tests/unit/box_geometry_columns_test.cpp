// SPDX-License-Identifier: AGPL-3.0-or-later

#include "storage/detail/BoxGeometryColumns.h"

#include "core/box.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>

namespace fmgr::storage::detail {
  namespace {

    // The reader of `outer_dimensions_json` must tolerate every value the write
    // path cannot produce but a row created outside the API can: the demo seed
    // wrote `{}` where `dimensions_dump()` stores the text `null`, and a SQL
    // NULL surfaces as empty text on the SQLite read path. All of them mean
    // "no dimensions", which is the state `dimensions_dump(nullopt)` writes.
    TEST(ParseDimensionsTest, ReturnsAbsentForEmptyText) {
      EXPECT_FALSE(parse_dimensions("").has_value());
    }

    TEST(ParseDimensionsTest, ReturnsAbsentForJsonNull) {
      EXPECT_FALSE(parse_dimensions("null").has_value());
    }

    TEST(ParseDimensionsTest, ReturnsAbsentForJsonStringNull) {
      EXPECT_FALSE(parse_dimensions("\"null\"").has_value());
    }

    TEST(ParseDimensionsTest, ReturnsAbsentForEmptyObject) {
      EXPECT_FALSE(parse_dimensions("{}").has_value());
    }

    TEST(ParseDimensionsTest, ReturnsAbsentForObjectMissingAField) {
      EXPECT_FALSE(parse_dimensions("{\"width\": 12.5}").has_value());
      EXPECT_FALSE(parse_dimensions("{\"width\": 12.5, \"height\": 40.0}").has_value());
      EXPECT_FALSE(parse_dimensions("{\"height\": 40.0, \"depth\": 12.5}").has_value());
    }

    TEST(ParseDimensionsTest, ReturnsAbsentForNonNumericFields) {
      EXPECT_FALSE(
          parse_dimensions("{\"width\": \"wide\", \"height\": 40.0, \"depth\": 12.5}").has_value());
      EXPECT_FALSE(
          parse_dimensions("{\"width\": 12.5, \"height\": null, \"depth\": 12.5}").has_value());
    }

    TEST(ParseDimensionsTest, ReturnsAbsentForNonObjectJson) {
      EXPECT_FALSE(parse_dimensions("[12.5, 40.0, 12.5]").has_value());
      EXPECT_FALSE(parse_dimensions("12.5").has_value());
      EXPECT_FALSE(parse_dimensions("true").has_value());
    }

    TEST(ParseDimensionsTest, ReturnsDimensionsForACompleteObject) {
      const auto dimensions =
          parse_dimensions("{\"width\": 12.5, \"height\": 40.0, \"depth\": 8.0}");
      ASSERT_TRUE(dimensions.has_value());
      EXPECT_EQ(dimensions->width, 12.5);
      EXPECT_EQ(dimensions->height, 40.0);
      EXPECT_EQ(dimensions->depth, 8.0);
    }

    TEST(ParseDimensionsTest, DumpAndParseRoundTrip) {
      const auto dimensions = parse_dimensions(
          dimensions_dump(core::OuterDimensionsMm{.width = 1.5, .height = 2.5, .depth = 3.5}));
      ASSERT_TRUE(dimensions.has_value());
      EXPECT_EQ(*dimensions, (core::OuterDimensionsMm{.width = 1.5, .height = 2.5, .depth = 3.5}));

      // The write path stores nullopt as the text `null`; parsing that must come
      // back as absent, not throw.
      EXPECT_FALSE(parse_dimensions(dimensions_dump(std::nullopt)).has_value());
    }

  } // namespace
} // namespace fmgr::storage::detail
