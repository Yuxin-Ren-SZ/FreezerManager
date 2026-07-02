// SPDX-License-Identifier: AGPL-3.0-or-later

#include "obs/Health.h"

#include <nlohmann/json.hpp>

#include <array>
#include <exception>
#include <string>
#include <string_view>
#include <utility>

namespace fmgr::obs {
  namespace {

    [[nodiscard]] std::string_view state_token(DepStatus::State state) {
      switch (state) {
      case DepStatus::State::Ok:
        return "ok";
      case DepStatus::State::Failed:
        return "failed";
      case DepStatus::State::Disabled:
        return "disabled";
      }
      return "failed";
    }

    // Invoke one probe, converting an escaping exception into a Failed status so
    // the endpoint always produces a verdict.
    [[nodiscard]] DepStatus run_probe(const std::function<DepStatus()>& probe) {
      if (!probe) {
        return DepStatus::disabled("probe not configured");
      }
      try {
        return probe();
      } catch (const std::exception& e) {
        return DepStatus::failed(e.what());
      } catch (...) {
        return DepStatus::failed("unknown error");
      }
    }

  } // namespace

  HealthReport check_health(const HealthProbe& probe) {
    const std::array<std::pair<std::string_view, DepStatus>, 3> checks{{
        {"database", run_probe(probe.database)},
        {"kms", run_probe(probe.kms)},
        {"backup", run_probe(probe.backup)},
    }};

    bool healthy = true;
    nlohmann::json checks_json = nlohmann::json::object();
    for (const auto& [name, status] : checks) {
      if (status.state == DepStatus::State::Failed) {
        healthy = false;
      }
      checks_json[std::string(name)] = {
          {"status", state_token(status.state)},
          {"detail", status.detail},
      };
    }

    nlohmann::json body{
        {"status", healthy ? "ok" : "unhealthy"},
        {"checks", std::move(checks_json)},
    };

    HealthReport report;
    report.healthy = healthy;
    report.http_status = healthy ? 200 : 503;
    report.json_body = body.dump();
    return report;
  }

} // namespace fmgr::obs
