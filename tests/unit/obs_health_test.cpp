// SPDX-License-Identifier: AGPL-3.0-or-later

#include "obs/Health.h"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <stdexcept>

namespace {

  using fmgr::obs::check_health;
  using fmgr::obs::DepStatus;
  using fmgr::obs::HealthProbe;

  HealthProbe all_ok() {
    return HealthProbe{
        .database = [] { return DepStatus::ok(); },
        .kms = [] { return DepStatus::ok("key_id=test"); },
        .backup = [] { return DepStatus::ok(); },
    };
  }

  // Every dependency healthy -> overall ok, HTTP 200, per-dep "ok".
  TEST(ObsHealth, AllOkIsHealthy200) {
    const auto report = check_health(all_ok());
    EXPECT_TRUE(report.healthy);
    EXPECT_EQ(report.http_status, 200);
    const auto json = nlohmann::json::parse(report.json_body);
    EXPECT_EQ(json.at("status"), "ok");
    EXPECT_EQ(json.at("checks").at("database").at("status"), "ok");
    EXPECT_EQ(json.at("checks").at("kms").at("status"), "ok");
    EXPECT_EQ(json.at("checks").at("backup").at("status"), "ok");
  }

  // A disabled dependency (e.g. PHI/KMS off, no backup target) is a valid
  // configuration, not a failure: the deployment stays healthy and probes 200.
  TEST(ObsHealth, DisabledDependencyDoesNotFailTheVerdict) {
    auto probe = all_ok();
    probe.kms = [] { return DepStatus::disabled("no master KEK configured"); };
    probe.backup = [] { return DepStatus::disabled("FMGR_BACKUP_DIR unset"); };
    const auto report = check_health(probe);
    EXPECT_TRUE(report.healthy);
    EXPECT_EQ(report.http_status, 200);
    const auto json = nlohmann::json::parse(report.json_body);
    EXPECT_EQ(json.at("status"), "ok");
    EXPECT_EQ(json.at("checks").at("kms").at("status"), "disabled");
    EXPECT_EQ(json.at("checks").at("backup").at("status"), "disabled");
  }

  // A failing dependency drives an unhealthy verdict + HTTP 503 for the probe.
  TEST(ObsHealth, FailedDependencyIsUnhealthy503) {
    auto probe = all_ok();
    probe.database = [] { return DepStatus::failed("database unreachable"); };
    const auto report = check_health(probe);
    EXPECT_FALSE(report.healthy);
    EXPECT_EQ(report.http_status, 503);
    const auto json = nlohmann::json::parse(report.json_body);
    EXPECT_EQ(json.at("status"), "unhealthy");
    EXPECT_EQ(json.at("checks").at("database").at("status"), "failed");
    EXPECT_EQ(json.at("checks").at("database").at("detail"), "database unreachable");
  }

  // A probe that throws is caught and reported as failed, never propagated — the
  // health endpoint must always answer.
  TEST(ObsHealth, ThrowingProbeIsReportedAsFailed) {
    auto probe = all_ok();
    probe.kms = []() -> DepStatus { throw std::runtime_error("kms exploded"); };
    const auto report = check_health(probe);
    EXPECT_FALSE(report.healthy);
    EXPECT_EQ(report.http_status, 503);
    const auto json = nlohmann::json::parse(report.json_body);
    EXPECT_EQ(json.at("checks").at("kms").at("status"), "failed");
    EXPECT_NE(std::string(json.at("checks").at("kms").at("detail")).find("kms exploded"),
              std::string::npos);
  }

} // namespace
