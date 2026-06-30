// SPDX-License-Identifier: AGPL-3.0-or-later

// Minimal Prometheus metrics registry (PRD §17). Hand-rolled rather than pulling
// prometheus-cpp, which bundles civetweb — a second HTTP server that collides
// with the Drogon front door we already serve /metrics from — for ~150 lines of
// trivial text-exposition format. Supports counters and latency histograms with
// labels; thread-safe so gRPC handler threads can update concurrently.
#ifndef FMGR_OBS_METRICS_H
#define FMGR_OBS_METRICS_H

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace fmgr::obs {

  class MetricsRegistry {
  public:
    using Labels = std::vector<std::pair<std::string, std::string>>;

    // Increment a counter series by `value` (default 1). `help` is recorded the
    // first time the family is seen.
    void inc_counter(const std::string& name, const std::string& help, const Labels& labels,
                     double value = 1.0);

    // Record one observation (seconds) into a latency histogram family with a
    // fixed bucket layout.
    void observe_latency(const std::string& name, const std::string& help, const Labels& labels,
                         double seconds);

    // Render the whole registry in Prometheus text-exposition format.
    [[nodiscard]] std::string render() const;

  private:
    struct CounterFamily {
      std::string help;
      std::map<std::string, double> series; // label-string -> value
    };
    struct HistogramSeries {
      std::vector<std::uint64_t> bucket_hits; // per-bound, non-cumulative
      double sum{0.0};
      std::uint64_t count{0};
    };
    struct HistogramFamily {
      std::string help;
      std::map<std::string, HistogramSeries> series;
    };

    mutable std::mutex mutex_;
    std::map<std::string, CounterFamily> counters_;
    std::map<std::string, HistogramFamily> histograms_;
  };

  // Process-wide default registry shared by the gRPC interceptor and /metrics.
  [[nodiscard]] MetricsRegistry& metrics();

} // namespace fmgr::obs

#endif // FMGR_OBS_METRICS_H
