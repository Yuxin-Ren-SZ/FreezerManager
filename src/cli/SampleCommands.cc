// SPDX-License-Identifier: AGPL-3.0-or-later

#include "cli/SampleCommands.h"

#include "cli/SampleCsv.h"
#include "core/enums.h"

#include <array>
#include <chrono>
#include <ctime>
#include <string>

namespace fmgr::cli {

  namespace {

    // Current wall-clock time as an RFC 3339 / ISO 8601 UTC string for the
    // export header. Seconds precision is plenty for chain-of-custody metadata.
    [[nodiscard]] std::string now_iso8601_utc() {
      const auto now = std::chrono::system_clock::now();
      const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
      std::tm utc{};
#if defined(_WIN32)
      gmtime_s(&utc, &seconds);
#else
      gmtime_r(&seconds, &utc);
#endif
      std::array<char, sizeof("2026-06-07T12:34:56Z")> buffer{};
      std::strftime(buffer.data(), buffer.size(), "%Y-%m-%dT%H:%M:%SZ", &utc);
      return std::string(buffer.data());
    }

  } // namespace

  void run_sample_list(storage::IStorageBackend& backend, const SampleQueryOptions& options,
                       std::ostream& out) {
    const auto samples = query_samples(backend, options);

    out << "ID\tSTATUS\tLOCATION\tNAME\n";
    for (const auto& sample : samples) {
      const std::string location =
          (sample.box_id.has_value() && sample.position_label.has_value())
              ? sample.box_id->to_string() + "/" + sample.position_label.value()
              : "-";
      out << sample.id.to_string() << '\t' << core::to_string(sample.status) << '\t' << location
          << '\t' << sample.name << '\n';
    }
    out << samples.size() << " sample(s)\n";
  }

  void run_sample_export(storage::IStorageBackend& backend, const SampleQueryOptions& options,
                         std::ostream& out) {
    const auto samples = query_samples(backend, options);
    write_sample_csv(out, samples, backend.current_version().value, options.lab_id.to_string(),
                     now_iso8601_utc());
  }

} // namespace fmgr::cli
