// SPDX-License-Identifier: AGPL-3.0-or-later

// Structured JSON logging (PRD §17). Every emitted line is one JSON object
// carrying a fixed schema contract — ts, level, msg, request_id, actor_user_id
// (nullable), lab_id (nullable), event — so a log pipeline (Loki / Elasticsearch
// / Splunk) can parse, index, and alert reliably.
//
// PHI safety: `LogFields` admits only scalar identifiers plus an event tag. There
// is intentionally no channel to pass an entity blob, so a sample's contents can
// never reach a log line through this surface (PRD §17, §20 risk row).
#ifndef FMGR_OBS_LOG_H
#define FMGR_OBS_LOG_H

#include <optional>
#include <string>
#include <string_view>

namespace fmgr::obs {

  enum class Level { Trace, Debug, Info, Warn, Error };

  // The structured context attached to a log line. `request_id` correlates a line
  // with the audit row + RPC that produced it; `event` is a stable dotted token
  // (e.g. "sample.create", "server.start") for indexing.
  struct LogFields {
    std::string request_id;
    std::optional<std::string> actor_user_id;
    std::optional<std::string> lab_id;
    std::string event;
  };

  // Pure: build one canonical JSON log line (no trailing newline). `ts` is the
  // caller-supplied RFC 3339 UTC timestamp, injected so the function is
  // deterministically testable. The message is JSON-escaped.
  [[nodiscard]] std::string format_log_line(std::string_view ts_rfc3339, Level level,
                                            std::string_view msg, const LogFields& fields);

  // Configure spdlog's default logger as an async, non-blocking JSON-line emitter
  // to stderr (journald-friendly). The pattern is "%v" so the only bytes written
  // are the JSON object built by format_log_line / the wrapped envelope. Lines
  // below `min_level` are dropped. Idempotent-safe to call once at startup.
  void init_logging(Level min_level = Level::Info);

  // Emit a structured line at runtime, stamping `ts` from the system clock.
  void log_event(Level level, std::string_view msg, const LogFields& fields);

  // Convenience for server-lifecycle lines that have no request/actor context
  // (startup, shutdown, scheduler state). Records only ts/level/msg/event with
  // null actor + lab and an empty request_id.
  void log_lifecycle(Level level, std::string_view msg, std::string_view event);

} // namespace fmgr::obs

#endif // FMGR_OBS_LOG_H
