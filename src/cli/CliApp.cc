// SPDX-License-Identifier: AGPL-3.0-or-later

#include "cli/CliApp.h"

#include "cli/AuditCommands.h"
#include "cli/BackendFactory.h"
#include "cli/NounCommands.h"
#include "cli/SampleCommands.h"
#include "cli/SampleQuery.h"
#include "core/ids.h"

#include <CLI/CLI.hpp>

#include <cstddef>
#include <exception>
#include <fstream>
#include <iostream>
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

    [[nodiscard]] NounListOptions to_noun_list_options(const CommonArgs& args) {
      NounListOptions options{.lab_id = core::LabId::parse(args.lab_id)};
      if (args.limit > 0) {
        options.limit = args.limit;
      }
      options.include_tombstoned = args.include_tombstoned;
      return options;
    }

    // One read noun (freezer/box/item-type) and its `list` + `inspect`
    // subcommands. The arg storage lives for the duration of run_cli so CLI11 can
    // write into it during parse.
    struct NounSubcommands {
      CommonArgs list_args;
      CommonArgs inspect_args; // only sqlite/postgres/lab are read for inspect
      std::string inspect_id;
      CLI::App* list{nullptr};
      CLI::App* inspect{nullptr};
    };

    void add_read_noun(CLI::App& app, const std::string& name, NounSubcommands& noun) {
      CLI::App* root = app.add_subcommand(name, name + " queries");
      root->require_subcommand(1);

      noun.list = root->add_subcommand("list", "List " + name + " rows in a lab as a table");
      add_common_args(noun.list, noun.list_args);

      noun.inspect = root->add_subcommand("inspect", "Show one " + name + " by --id");
      noun.inspect->add_option("--sqlite", noun.inspect_args.sqlite_path,
                               "Path to a SQLite database file");
      noun.inspect->add_option("--postgres", noun.inspect_args.postgres_url,
                               "PostgreSQL connection URL");
      noun.inspect->add_option("--lab", noun.inspect_args.lab_id, "Lab UUID to scope the query to")
          ->required();
      noun.inspect->add_option("--id", noun.inspect_id, "Entity UUID to inspect")->required();
    }

    // Run a read noun's `list` or `inspect` if it was the parsed subcommand.
    // Returns the exit code when handled, or std::nullopt when neither child of
    // this noun was selected. `inspect_fn` receives the raw --id string so each
    // noun can parse it into its own strong-id type. Keeps run_cli's dispatch
    // ladder flat (one call per noun) rather than two if-blocks each.
    template <typename ListFn, typename InspectFn>
    [[nodiscard]] std::optional<int> dispatch_read_noun(const NounSubcommands& noun, ListFn list_fn,
                                                        InspectFn inspect_fn, std::ostream& out) {
      if (noun.list->parsed()) {
        auto backend = open_backend(to_backend_options(noun.list_args));
        list_fn(*backend, to_noun_list_options(noun.list_args), out);
        return 0;
      }
      if (noun.inspect->parsed()) {
        auto backend = open_backend(to_backend_options(noun.inspect_args));
        return inspect_fn(*backend, core::LabId::parse(noun.inspect_args.lab_id), noun.inspect_id,
                          out);
      }
      return std::nullopt;
    }

    // Dispatch any of the three read nouns. Returns the exit code when one of them
    // was the parsed command, or std::nullopt otherwise. Keeps the per-noun
    // wiring out of run_cli (cognitive-complexity budget).
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    [[nodiscard]] std::optional<int> dispatch_read_nouns(const NounSubcommands& freezer,
                                                         const NounSubcommands& box,
                                                         const NounSubcommands& item_type,
                                                         std::ostream& out) {
      if (const auto code = dispatch_read_noun(
              freezer, run_freezer_list,
              [](storage::IStorageBackend& backend, core::LabId lab, const std::string& id,
                 std::ostream& sink) {
                return run_freezer_inspect(backend, lab, core::FreezerId::parse(id), sink);
              },
              out)) {
        return code;
      }
      if (const auto code = dispatch_read_noun(
              box, run_box_list,
              [](storage::IStorageBackend& backend, core::LabId lab, const std::string& id,
                 std::ostream& sink) {
                return run_box_inspect(backend, lab, core::BoxId::parse(id), sink);
              },
              out)) {
        return code;
      }
      return dispatch_read_noun(
          item_type, run_item_type_list,
          [](storage::IStorageBackend& backend, core::LabId lab, const std::string& id,
             std::ostream& sink) {
            return run_item_type_inspect(backend, lab, core::ItemTypeId::parse(id), sink);
          },
          out);
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

    // import: transactional CSV import with a dry-run validation mode. Does not
    // reuse add_common_args (the --limit / --include-tombstoned flags are
    // meaningless for a write path); it takes its own --lab, --actor, and file.
    std::string import_sqlite;
    std::string import_postgres;
    std::string import_lab;
    std::string import_actor;
    std::string import_file;
    bool import_dry_run = false;
    CLI::App* importer = sample->add_subcommand("import", "Import samples from a CSV file");
    importer->add_option("--sqlite", import_sqlite, "Path to a SQLite database file");
    importer->add_option("--postgres", import_postgres, "PostgreSQL connection URL");
    importer->add_option("--lab", import_lab, "Lab UUID to import the samples into")->required();
    importer->add_option("--actor", import_actor, "User UUID recorded as the importer")->required();
    importer->add_flag("--dry-run", import_dry_run,
                       "Validate every row and report, but do not write anything");
    importer->add_option("file", import_file, "CSV file to import ('-' for stdin)")->required();

    CLI::App* audit = app.add_subcommand("audit", "Audit log commands");
    audit->require_subcommand(1);
    std::string audit_sqlite;
    std::string audit_postgres;
    CLI::App* verify = audit->add_subcommand("verify", "Verify the audit hash chain (global)");
    verify->add_option("--sqlite", audit_sqlite, "Path to a SQLite database file");
    verify->add_option("--postgres", audit_postgres, "PostgreSQL connection URL");

    NounSubcommands freezer_noun;
    NounSubcommands box_noun;
    NounSubcommands item_type_noun;
    add_read_noun(app, "freezer", freezer_noun);
    add_read_noun(app, "box", box_noun);
    add_read_noun(app, "item-type", item_type_noun);

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
      if (importer->parsed()) {
        BackendOptions backend_opts;
        if (!import_sqlite.empty()) {
          backend_opts.sqlite_path = import_sqlite;
        }
        if (!import_postgres.empty()) {
          backend_opts.postgres_url = import_postgres;
        }
        auto backend = open_backend(backend_opts);
        const SampleImportOptions import_opts{.lab_id = core::LabId::parse(import_lab),
                                              .actor = core::UserId::parse(import_actor),
                                              .dry_run = import_dry_run};
        if (import_file == "-") {
          return run_sample_import(*backend, import_opts, std::cin, out);
        }
        std::ifstream file(import_file, std::ios::binary);
        if (!file) {
          err << "error: cannot open input file: " << import_file << '\n';
          return 1;
        }
        return run_sample_import(*backend, import_opts, file, out);
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
      if (const auto code = dispatch_read_nouns(freezer_noun, box_noun, item_type_noun, out)) {
        return *code;
      }
    } catch (const std::exception& error) {
      err << "error: " << error.what() << '\n';
      return 1;
    }

    return 0;
  }

} // namespace fmgr::cli
