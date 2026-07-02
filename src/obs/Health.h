// SPDX-License-Identifier: AGPL-3.0-or-later

// Dependency health checks for the /health endpoint (PRD §17). Each dependency
// (database, KMS, backup target) is probed by a caller-supplied callable so the
// pure verdict logic here stays free of I/O and is unit-testable; the concrete
// probes are wired in main.cc where the backend, KMS, and backup dir are in
// scope. The endpoint is suitable for a load-balancer readiness probe.
#ifndef FMGR_OBS_HEALTH_H
#define FMGR_OBS_HEALTH_H

#include <functional>
#include <string>
#include <utility>

namespace fmgr::obs {

  // The state of one dependency. `Disabled` means "configured off" (e.g. PHI/KMS
  // not enabled, no backup target) — a valid deployment, not a failure.
  struct DepStatus {
    enum class State { Ok, Failed, Disabled };
    State state{State::Ok};
    std::string detail;

    [[nodiscard]] static DepStatus ok(std::string detail = "") {
      return {State::Ok, std::move(detail)};
    }
    [[nodiscard]] static DepStatus failed(std::string detail) {
      return {State::Failed, std::move(detail)};
    }
    [[nodiscard]] static DepStatus disabled(std::string detail = "") {
      return {State::Disabled, std::move(detail)};
    }
  };

  // The three dependency probes. Each is invoked once per /health request. A probe
  // that throws is treated as Failed (the endpoint must always answer).
  struct HealthProbe {
    std::function<DepStatus()> database;
    std::function<DepStatus()> kms;
    std::function<DepStatus()> backup;
  };

  struct HealthReport {
    bool healthy{true};    // false iff any dependency Failed (Disabled is fine)
    int http_status{200};  // 200 when healthy, else 503
    std::string json_body; // {"status":..., "checks":{name:{status,detail}}}
  };

  // Run every probe, build the structured report, and decide the verdict. Pure
  // w.r.t. its argument: all I/O lives inside the supplied callables.
  [[nodiscard]] HealthReport check_health(const HealthProbe& probe);

} // namespace fmgr::obs

#endif // FMGR_OBS_HEALTH_H
