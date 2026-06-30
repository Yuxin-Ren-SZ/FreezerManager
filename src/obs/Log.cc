// SPDX-License-Identifier: AGPL-3.0-or-later

#include "obs/Log.h"

#include <nlohmann/json.hpp>
#include <spdlog/async.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <string_view>

namespace fmgr::obs {
  namespace {

    [[nodiscard]] std::string_view level_token(Level level) {
      switch (level) {
      case Level::Trace:
        return "trace";
      case Level::Debug:
        return "debug";
      case Level::Info:
        return "info";
      case Level::Warn:
        return "warn";
      case Level::Error:
        return "error";
      }
      return "info";
    }

    [[nodiscard]] spdlog::level::level_enum to_spdlog(Level level) {
      switch (level) {
      case Level::Trace:
        return spdlog::level::trace;
      case Level::Debug:
        return spdlog::level::debug;
      case Level::Info:
        return spdlog::level::info;
      case Level::Warn:
        return spdlog::level::warn;
      case Level::Error:
        return spdlog::level::err;
      }
      return spdlog::level::info;
    }

    // RFC 3339 UTC with microsecond precision: 2026-06-29T12:34:56.000007Z.
    [[nodiscard]] std::string now_rfc3339() {
      const auto now = std::chrono::system_clock::now();
      const auto since_epoch = now.time_since_epoch();
      const auto secs = std::chrono::duration_cast<std::chrono::seconds>(since_epoch);
      const auto micros =
          std::chrono::duration_cast<std::chrono::microseconds>(since_epoch - secs).count();

      const std::time_t time = secs.count();
      std::tm utc{};
      gmtime_r(&time, &utc);

      std::array<char, 32> date{};
      std::strftime(date.data(), date.size(), "%Y-%m-%dT%H:%M:%S", &utc);

      std::array<char, 48> out{};
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
      std::snprintf(out.data(), out.size(), "%s.%06lldZ", date.data(),
                    static_cast<long long>(micros));
      return {out.data()};
    }

  } // namespace

  std::string format_log_line(std::string_view ts_rfc3339, Level level, std::string_view msg,
                              const LogFields& fields) {
    nlohmann::json line;
    line["ts"] = ts_rfc3339;
    line["level"] = level_token(level);
    line["msg"] = msg;
    line["request_id"] = fields.request_id;
    line["actor_user_id"] =
        fields.actor_user_id.has_value() ? nlohmann::json(*fields.actor_user_id) : nlohmann::json();
    line["lab_id"] = fields.lab_id.has_value() ? nlohmann::json(*fields.lab_id) : nlohmann::json();
    line["event"] = fields.event;
    // Compact, one object per line; non-ASCII left as UTF-8 (no \u escaping).
    return line.dump();
  }

  void init_logging(Level min_level) {
    // Async logger with a bounded queue: log writes never block the gRPC thread
    // pool, and an error flood drops the oldest lines instead of growing memory
    // unbounded. Set as the default so spdlog::* and log_event() route here.
    constexpr std::size_t k_log_queue_size = 8192;
    spdlog::init_thread_pool(k_log_queue_size, 1);
    auto logger = spdlog::create_async_nb<spdlog::sinks::stderr_color_sink_mt>("freezerd");
    // "%v" => emit only the message bytes; the JSON envelope is built by
    // format_log_line, so no spdlog prefix corrupts the one-object-per-line shape.
    logger->set_pattern("%v");
    spdlog::set_default_logger(logger);
    spdlog::set_level(to_spdlog(min_level));
    spdlog::flush_on(spdlog::level::err);
  }

  void log_event(Level level, std::string_view msg, const LogFields& fields) {
    spdlog::log(to_spdlog(level), "{}", format_log_line(now_rfc3339(), level, msg, fields));
  }

  void log_lifecycle(Level level, std::string_view msg, std::string_view event) {
    log_event(level, msg, LogFields{.request_id = "", .event = std::string(event)});
  }

} // namespace fmgr::obs
