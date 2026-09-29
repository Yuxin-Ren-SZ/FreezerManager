// SPDX-License-Identifier: AGPL-3.0-or-later

#include "cli/SampleCommands.h"

#include "cli/CsvReader.h"
#include "cli/SampleCsv.h"
#include "cli/SampleImport.h"
#include "core/enums.h"
#include "storage/CustomFieldWrite.h"
#include "storage/SampleTraits.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace fmgr::cli {

  namespace {

    // Current wall-clock time as an RFC 3339 / ISO 8601 UTC string for the
    // export header. Seconds precision is plenty for chain-of-custody metadata.
    [[nodiscard]] std::string now_iso8601_utc() {
      const auto now = std::chrono::system_clock::now();
      const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
      std::tm utc{};
#ifdef _WIN32
      gmtime_s(&utc, &seconds);
#else
      gmtime_r(&seconds, &utc);
#endif
      std::array<char, sizeof("2026-06-07T12:34:56Z")> buffer{};
      std::strftime(buffer.data(), buffer.size(), "%Y-%m-%dT%H:%M:%SZ", &utc);
      return {buffer.data()};
    }

    // Wall-clock now as a core::Timestamp (UTC microseconds) for created_at /
    // last_modified_at on imported samples.
    [[nodiscard]] core::Timestamp now_timestamp() {
      const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();
      return core::Timestamp::from_unix_micros(static_cast<std::int64_t>(micros));
    }

    [[nodiscard]] storage::MutationContext
    import_mutation_context(const SampleImportOptions& opts) {
      return storage::MutationContext{
          .actor_user_id = opts.actor,
          .actor_session_id = "freezerctl-import",
          .request_id = "",
          .reason = "csv_import",
          .lab_id = opts.lab_id.to_string(),
      };
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

  int run_sample_import(storage::IStorageBackend& backend, const SampleImportOptions& options,
                        std::istream& input, std::ostream& out) {
    const auto records = parse_csv(input); // CsvParseError propagates to run_cli's err handler
    const ImportContext ctx{
        .lab_id = options.lab_id, .actor = options.actor, .now = now_timestamp()};
    const ImportReport report = build_import(records, ctx);

    if (!report.header_error.empty()) {
      out << "error: " << report.header_error << '\n';
      return 1;
    }

    // Per-row structural report first, regardless of mode.
    for (const auto& row : report.rows) {
      if (row.ok && row.sample.has_value()) {
        out << "row " << row.row_number << ": OK    " << row.sample.value().name << '\n';
      } else {
        out << "row " << row.row_number << ": ERROR " << row.error << '\n';
      }
    }
    if (report.has_errors()) {
      out << report.error_count() << " row(s) failed validation; nothing written\n";
      return 1;
    }

    // RLS gate: scope every insert to the target lab so the samples policy (and
    // the cross-lab integrity checks in the repository) see the right context.
    const auto inject_lab = [&](storage::ITransaction& txn) {
      txn.set_session_var("current_lab_ids", options.lab_id.to_string());
    };

    // Split each row the same way the server splits a CreateSample/UpdateSample
    // request, so a PHI-tagged key that arrived in the CSV's custom_fields_json
    // cell reaches the encrypted envelope and never the plaintext column — this
    // tool writes into the same database the server reads (#108). Throws for a
    // row that cannot be stored safely (validation failure, PHI with no master
    // key, PHI where the lab has PHI mode off); the caller reports the row and
    // never commits.
    const auto prepare_row = [&](storage::ITransaction& txn, core::Sample& sample) {
      const auto prepared = storage::prepare_custom_fields(txn, options.lab_id, sample.item_type_id,
                                                           sample.custom_fields_json, options.kms);
      sample.custom_fields_json = prepared.custom_fields_json;
      sample.phi_fields_enc_json = prepared.phi_fields_enc_json;
    };

    if (options.dry_run) {
      // Validate against committed DB state without persisting: each row inserts
      // in its own transaction that is dropped (rolled back), so one failing row
      // cannot poison the validation of the others.
      std::size_t db_errors = 0;
      for (const auto& row : report.rows) {
        if (!row.sample.has_value()) {
          continue;
        }
        try {
          auto txn = backend.begin(storage::IsolationLevel::Serializable);
          inject_lab(*txn);
          core::Sample sample = row.sample.value();
          prepare_row(*txn, sample);
          txn->repo<core::Sample>().insert(sample, import_mutation_context(options));
          // Intentionally no commit(): the transaction rolls back on destruction.
        } catch (const std::exception& error) {
          ++db_errors;
          out << "row " << row.row_number << ": ERROR " << error.what() << '\n';
        }
      }
      if (db_errors > 0) {
        out << db_errors << " row(s) failed validation; dry-run, nothing written\n";
        return 1;
      }
      out << "dry-run OK: " << report.rows.size() << " row(s) valid; nothing written\n";
      return 0;
    }

    // Real import: all-or-nothing in a single transaction. Any insert/commit
    // failure aborts the whole batch (the transaction is never committed).
    auto txn = backend.begin(storage::IsolationLevel::Serializable);
    inject_lab(*txn);
    std::size_t db_errors = 0;
    for (const auto& row : report.rows) {
      if (!row.sample.has_value()) {
        continue;
      }
      try {
        core::Sample sample = row.sample.value();
        prepare_row(*txn, sample);
        txn->repo<core::Sample>().insert(sample, import_mutation_context(options));
      } catch (const std::exception& error) {
        ++db_errors;
        out << "row " << row.row_number << ": ERROR " << error.what() << '\n';
      }
    }
    if (db_errors > 0) {
      out << db_errors << " row(s) failed validation; nothing written\n";
      return 1;
    }
    txn->commit();
    out << "imported " << report.rows.size() << " sample(s)\n";
    return 0;
  }

} // namespace fmgr::cli
