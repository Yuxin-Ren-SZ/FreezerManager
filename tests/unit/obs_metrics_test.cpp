// SPDX-License-Identifier: AGPL-3.0-or-later

#include "obs/Metrics.h"

#include <gtest/gtest.h>

#include <string>

namespace {

  using fmgr::obs::MetricsRegistry;

  // Counters accumulate per label set and render in Prometheus text format with a
  // HELP + TYPE header.
  TEST(ObsMetrics, CounterAccumulatesAndRenders) {
    MetricsRegistry reg;
    reg.inc_counter("rpc_requests_total", "Total RPCs.", {{"method", "Create"}, {"code", "OK"}});
    reg.inc_counter("rpc_requests_total", "Total RPCs.", {{"method", "Create"}, {"code", "OK"}});
    reg.inc_counter("rpc_requests_total", "Total RPCs.", {{"method", "Get"}, {"code", "OK"}});

    const auto out = reg.render();
    EXPECT_NE(out.find("# HELP rpc_requests_total Total RPCs."), std::string::npos);
    EXPECT_NE(out.find("# TYPE rpc_requests_total counter"), std::string::npos);
    EXPECT_NE(out.find(R"(rpc_requests_total{method="Create",code="OK"} 2)"), std::string::npos);
    EXPECT_NE(out.find(R"(rpc_requests_total{method="Get",code="OK"} 1)"), std::string::npos);
  }

  // A histogram emits cumulative _bucket lines (incl. +Inf), a _sum, and a _count.
  TEST(ObsMetrics, HistogramRendersBucketsSumCount) {
    MetricsRegistry reg;
    reg.observe_latency("rpc_latency_seconds", "RPC latency.", {{"method", "Create"}}, 0.003);
    reg.observe_latency("rpc_latency_seconds", "RPC latency.", {{"method", "Create"}}, 0.2);

    const auto out = reg.render();
    EXPECT_NE(out.find("# TYPE rpc_latency_seconds histogram"), std::string::npos);
    // 0.003 falls in le=0.005; both observations are <= +Inf.
    EXPECT_NE(out.find(R"(rpc_latency_seconds_bucket{method="Create",le="0.005"} 1)"),
              std::string::npos);
    EXPECT_NE(out.find(R"(rpc_latency_seconds_bucket{method="Create",le="+Inf"} 2)"),
              std::string::npos);
    EXPECT_NE(out.find(R"(rpc_latency_seconds_count{method="Create"} 2)"), std::string::npos);
    EXPECT_NE(out.find(R"(rpc_latency_seconds_sum{method="Create"} 0.203)"), std::string::npos);
  }

  // Label values are escaped so a stray quote/backslash/newline cannot corrupt the
  // exposition format.
  TEST(ObsMetrics, EscapesLabelValues) {
    MetricsRegistry reg;
    reg.inc_counter("c_total", "h", {{"k", R"(a"b\c)"}});
    const auto out = reg.render();
    EXPECT_NE(out.find(R"(c_total{k="a\"b\\c"} 1)"), std::string::npos);
  }

} // namespace
