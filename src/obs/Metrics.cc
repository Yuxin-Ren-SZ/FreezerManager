// SPDX-License-Identifier: AGPL-3.0-or-later

#include "obs/Metrics.h"

#include <fmt/format.h>

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace fmgr::obs {
  namespace {

    // Default latency buckets in seconds (Prometheus client convention).
    constexpr std::array<double, 11> k_latency_bounds{0.005, 0.01, 0.025, 0.05, 0.1, 0.25,
                                                      0.5,   1.0,  2.5,   5.0,  10.0};

    void escape_into(std::string& out, std::string_view value) {
      for (const char ch : value) {
        switch (ch) {
        case '\\':
          out += "\\\\";
          break;
        case '"':
          out += "\\\"";
          break;
        case '\n':
          out += "\\n";
          break;
        default:
          out += ch;
        }
      }
    }

    // Render label pairs into the inner form `k1="v1",k2="v2"` (no braces). Used
    // both as the series map key and in the exposition output, so the key order is
    // the caller's insertion order — deterministic output.
    [[nodiscard]] std::string label_key(const MetricsRegistry::Labels& labels) {
      std::string out;
      for (std::size_t i = 0; i < labels.size(); ++i) {
        if (i != 0) {
          out += ',';
        }
        out += labels[i].first;
        out += "=\"";
        escape_into(out, labels[i].second);
        out += '"';
      }
      return out;
    }

    // Wrap an inner label string in braces, or empty when there are no labels.
    [[nodiscard]] std::string braces(const std::string& inner) {
      return inner.empty() ? std::string{} : "{" + inner + "}";
    }

    [[nodiscard]] std::string num(double value) {
      return fmt::format("{}", value);
    }

  } // namespace

  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  void MetricsRegistry::inc_counter(const std::string& name, const std::string& help,
                                    const Labels& labels, double value) {
    const std::lock_guard<std::mutex> lock(mutex_);
    auto& family = counters_[name];
    if (family.help.empty()) {
      family.help = help;
    }
    family.series[label_key(labels)] += value;
  }

  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  void MetricsRegistry::observe_latency(const std::string& name, const std::string& help,
                                        const Labels& labels, double seconds) {
    const std::lock_guard<std::mutex> lock(mutex_);
    auto& family = histograms_[name];
    if (family.help.empty()) {
      family.help = help;
    }
    auto& series = family.series[label_key(labels)];
    if (series.bucket_hits.empty()) {
      series.bucket_hits.resize(k_latency_bounds.size(), 0);
    }
    for (std::size_t i = 0; i < k_latency_bounds.size(); ++i) {
      if (seconds <= k_latency_bounds[i]) {
        series.bucket_hits[i] += 1;
        break;
      }
    }
    series.sum += seconds;
    series.count += 1;
  }

  std::string MetricsRegistry::render() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::string out;

    // Append a "# HELP <name> <help>\n# TYPE <name> <type>\n" family header.
    const auto header = [&out](const std::string& name, const std::string& help,
                               std::string_view type) {
      out.append("# HELP ").append(name).append(" ").append(help).append("\n");
      out.append("# TYPE ").append(name).append(" ").append(type).append("\n");
    };
    // Append `le="<bound>"` (or the bare bound for +Inf) prefixed by the series
    // labels, into a histogram bucket's label set.
    const auto bucket_labels = [&out](const std::string& labels, std::string_view bound) {
      if (!labels.empty()) {
        out.append(labels).append(",");
      }
      out.append("le=\"").append(bound).append("\"");
    };

    for (const auto& [name, family] : counters_) {
      header(name, family.help, "counter");
      for (const auto& [labels, value] : family.series) {
        out.append(name).append(braces(labels)).append(" ").append(num(value)).append("\n");
      }
    }

    for (const auto& [name, family] : histograms_) {
      header(name, family.help, "histogram");
      for (const auto& [labels, series] : family.series) {
        std::uint64_t cumulative = 0;
        for (std::size_t i = 0; i < k_latency_bounds.size(); ++i) {
          cumulative += series.bucket_hits[i];
          out.append(name).append("_bucket{");
          bucket_labels(labels, num(k_latency_bounds[i]));
          out.append("} ").append(std::to_string(cumulative)).append("\n");
        }
        out.append(name).append("_bucket{");
        bucket_labels(labels, "+Inf");
        out.append("} ").append(std::to_string(series.count)).append("\n");
        out.append(name)
            .append("_sum")
            .append(braces(labels))
            .append(" ")
            .append(num(series.sum))
            .append("\n");
        out.append(name)
            .append("_count")
            .append(braces(labels))
            .append(" ")
            .append(std::to_string(series.count))
            .append("\n");
      }
    }

    return out;
  }

  MetricsRegistry& metrics() {
    static MetricsRegistry registry;
    return registry;
  }

} // namespace fmgr::obs
