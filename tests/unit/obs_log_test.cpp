// SPDX-License-Identifier: AGPL-3.0-or-later

#include "obs/Log.h"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <optional>
#include <string>

namespace {

  using fmgr::obs::format_log_line;
  using fmgr::obs::Level;
  using fmgr::obs::LogFields;

  // A line is one JSON object carrying exactly the PRD §17 schema contract.
  TEST(ObsLog, EmitsSchemaContractKeys) {
    const LogFields fields{
        .request_id = "req-123",
        .actor_user_id = "user-9",
        .lab_id = "lab-7",
        .event = "sample.create",
    };
    const auto line =
        format_log_line("2026-06-29T12:34:56.000007Z", Level::Info, "created sample", fields);

    const auto json = nlohmann::json::parse(line);
    EXPECT_EQ(json.at("ts"), "2026-06-29T12:34:56.000007Z");
    EXPECT_EQ(json.at("level"), "info");
    EXPECT_EQ(json.at("msg"), "created sample");
    EXPECT_EQ(json.at("request_id"), "req-123");
    EXPECT_EQ(json.at("actor_user_id"), "user-9");
    EXPECT_EQ(json.at("lab_id"), "lab-7");
    EXPECT_EQ(json.at("event"), "sample.create");
  }

  // Each level renders to its lowercase token.
  TEST(ObsLog, RendersLevelTokens) {
    const LogFields fields{.request_id = "", .event = "lifecycle"};
    for (const auto& [level, token] :
         {std::pair{Level::Trace, "trace"}, std::pair{Level::Debug, "debug"},
          std::pair{Level::Info, "info"}, std::pair{Level::Warn, "warn"},
          std::pair{Level::Error, "error"}}) {
      const auto json = nlohmann::json::parse(format_log_line("t", level, "m", fields));
      EXPECT_EQ(json.at("level"), token);
    }
  }

  // Nullable fields serialize to JSON null, never an empty string or a dropped key,
  // so a log pipeline can rely on a stable shape.
  TEST(ObsLog, NullableFieldsBecomeJsonNull) {
    const LogFields fields{
        .request_id = "req-1",
        .actor_user_id = std::nullopt,
        .lab_id = std::nullopt,
        .event = "auth.login",
    };
    const auto json = nlohmann::json::parse(format_log_line("t", Level::Warn, "m", fields));
    EXPECT_TRUE(json.at("actor_user_id").is_null());
    EXPECT_TRUE(json.at("lab_id").is_null());
  }

  // The message is JSON-escaped, so an embedded quote/brace cannot break the line
  // or smuggle a second object onto it.
  TEST(ObsLog, EscapesMessageContent) {
    const LogFields fields{.request_id = "", .event = "e"};
    const auto line = format_log_line("t", Level::Error, R"(boom "quote" and {brace})", fields);
    // Parses as a single object — no injection.
    const auto json = nlohmann::json::parse(line);
    EXPECT_EQ(json.at("msg"), R"(boom "quote" and {brace})");
  }

  // PHI guard: the field type only admits scalar identifiers + an event tag, so
  // there is no channel to pass a sample blob. The emitted object has exactly the
  // seven schema keys and nothing else.
  TEST(ObsLog, CarriesOnlySchemaKeys) {
    const LogFields fields{.request_id = "r", .actor_user_id = "u", .lab_id = "l", .event = "e"};
    const auto json = nlohmann::json::parse(format_log_line("t", Level::Info, "m", fields));
    EXPECT_EQ(json.size(), 7U);
  }

} // namespace
