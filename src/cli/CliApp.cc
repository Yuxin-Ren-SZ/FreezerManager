// SPDX-License-Identifier: AGPL-3.0-or-later

#include "cli/CliApp.h"

#include "cli/AuditCommands.h"
#include "cli/BackendFactory.h"
#include "cli/SampleCommands.h"
#include "cli/SampleQuery.h"
#include "core/ids.h"

#include <CLI/CLI.hpp>

#include <cstddef>
#include <exception>
#include <fstream>
#include <optional>
#include <string>

namespace fmgr::cli {

  namespace {

    // Flags shared by every sample subcommand. Bound onto each CLI11 subcommand
    // and read after parse.
    struct CommonArgs {
      std::string sqlite_path;
      std::string postgres_url;
      std::string lab_id;
      std::size_t limit{0}; // 0 = unlimited
      bool include_tombstoned{false};
    };

    void add_common_args(CLI::App* sub, CommonArgs& args) {
      sub->add_option("--sqlite", args.sqlite_path, "Path to a SQLite database file");
      sub->add_option("--postgres", args.postgres_url, "PostgreSQL connection URL");
      sub->add_option("--lab", args.lab_id, "Lab UUID to scope the query to")->required();
      sub->add_option("--limit", args.limit, "Maximum number of samples (0 = unlimited)");
      sub->add_flag("--include-tombstoned", args.include_tombstoned,
                    "Include soft-deleted (tombstoned) samples");
    }

    [[nodiscard]] BackendOptions to_backend_options(const CommonArgs& args) {
      BackendOptions options;
      if (!args.sqlite_path.empty()) {
        options.sqlite_path = args.sqlite_path;
      }
      if (!args.postgres_url.empty()) {
        options.postgres_url = args.postgres_url;
      }
      return options;
    }

    [[nodiscard]] SampleQueryOptions to_query_options(const CommonArgs& args) {
      SampleQueryOptions options{.lab_id = core::LabId::parse(args.lab_id)};
      if (args.limit > 0) {
        options.limit = args.limit;
      }
      options.include_tombstoned = args.include_tombstoned;
      return options;
    }

  } // namespace

  // out/err are conventional stream params (mirrors main()); order is unambiguous.
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  int run_cli(int argc, const char* const* argv, std::ostream& out, std::ostream& err) {
    CLI::App app{"freezerctl — FreezerManager command-line client"};
    app.require_subcommand(1);

    CLI::App* sample = app.add_subcommand("sample", "Sample queries");
    sample->require_subcommand(1);

    CommonArgs list_args;
    CLI::App* list = sample->add_subcommand("list", "List samples in a lab as a table");
    add_common_args(list, list_args);

    CommonArgs export_args;
    std::string export_out;
    CLI::App* exporter =
        sample->add_subcommand("export", "Export samples in a lab as chain-of-custody CSV");
    add_common_args(exporter, export_args);
    exporter->add_option("--out", export_out, "Write CSV to this file instead of stdout");

    CLI::App* audit = app.add_subcommand("audit", "Audit log commands");
    audit->require_subcommand(1);
    std::string audit_sqlite;
    std::string audit_postgres;
    CLI::App* verify = audit->add_subcommand("verify", "Verify the audit hash chain (global)");
    verify->add_option("--sqlite", audit_sqlite, "Path to a SQLite database file");
    verify->add_option("--postgres", audit_postgres, "PostgreSQL connection URL");

    try {
      app.parse(argc, argv);
    } catch (const CLI::ParseError& error) {
      return app.exit(error); // prints help/usage to stdout/stderr as appropriate
    }

    try {
      if (list->parsed()) {
        auto backend = open_backend(to_backend_options(list_args));
        run_sample_list(*backend, to_query_options(list_args), out);
        return 0;
      }
      if (exporter->parsed()) {
        auto backend = open_backend(to_backend_options(export_args));
        const auto query_options = to_query_options(export_args);
        if (export_out.empty()) {
          run_sample_export(*backend, query_options, out);
        } else {
          std::ofstream file(export_out, std::ios::binary | std::ios::trunc);
          if (!file) {
            err << "error: cannot open output file: " << export_out << '\n';
            return 1;
          }
          run_sample_export(*backend, query_options, file);
        }
        return 0;
      }
      if (verify->parsed()) {
        BackendOptions options;
        if (!audit_sqlite.empty()) {
          options.sqlite_path = audit_sqlite;
        }
        if (!audit_postgres.empty()) {
          options.postgres_url = audit_postgres;
        }
        auto backend = open_backend(options);
        return run_audit_verify(*backend, AuditVerifyOptions{}, out);
      }
    } catch (const std::exception& error) {
      err << "error: " << error.what() << '\n';
      return 1;
    }

    return 0;
  }

} // namespace fmgr::cli
