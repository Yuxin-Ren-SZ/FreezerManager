// SPDX-License-Identifier: AGPL-3.0-or-later

#include "cli/CliApp.h"

#include "backup/BackupCommands.h"
#include "backup/BackupRunner.h"
#include "cli/AuditCommands.h"
#include "cli/BackendFactory.h"
#include "cli/BoxImport.h"
#include "cli/CreateCommands.h"
#include "cli/CsvImport.h"
#include "cli/CustomFieldDefImport.h"
#include "cli/ItemTypeImport.h"
#include "cli/KeyCommands.h"
#include "cli/LabCommands.h"
#include "cli/NounCommands.h"
#include "cli/SampleCommands.h"
#include "cli/SampleQuery.h"
#include "cli/UserCommands.h"
#include "cli/UserImport.h"
#include "core/box.h"
#include "core/enums.h"
#include "core/identity.h"
#include "core/ids.h"
#include "core/item_type.h"
#include "kms/KmsFactory.h"
#include "storage/BoxGeometryTraits.h"
#include "storage/IdentityTraits.h"
#include "storage/ItemTypeTraits.h"

#include <CLI/CLI.hpp>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
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

    // Build backend options from a raw --sqlite / --postgres pair. open_backend()
    // enforces the exactly-one rule; an empty string means "flag absent".
    [[nodiscard]] BackendOptions backend_options_from(const std::string& sqlite_path,
                                                      const std::string& postgres_url) {
      BackendOptions options;
      if (!sqlite_path.empty()) {
        options.sqlite_path = sqlite_path;
      }
      if (!postgres_url.empty()) {
        options.postgres_url = postgres_url;
      }
      return options;
    }

    [[nodiscard]] BackendOptions to_backend_options(const CommonArgs& args) {
      return backend_options_from(args.sqlite_path, args.postgres_url);
    }

    // `key rotate` handler, factored out of run_cli to keep its cognitive
    // complexity in budget. Returns nullopt when the subcommand was not parsed.
    // NOLINTBEGIN(bugprone-easily-swappable-parameters)
    [[nodiscard]] std::optional<int> dispatch_key_rotate(bool parsed, const std::string& sqlite,
                                                         const std::string& postgres,
                                                         const std::string& lab,
                                                         const std::string& actor,
                                                         std::ostream& out, std::ostream& err) {
      // NOLINTEND(bugprone-easily-swappable-parameters)
      if (!parsed) {
        return std::nullopt;
      }
      auto backend = open_backend(backend_options_from(sqlite, postgres));
      auto kms = kms::make_default_kms();
      if (kms == nullptr) {
        err << "error: no master KEK configured (set CREDENTIALS_DIRECTORY/master_kek "
               "or FMGR_MASTER_KEK)\n";
        return 1;
      }
      std::optional<core::LabId> lab_filter;
      if (!lab.empty()) {
        lab_filter = core::LabId::parse(lab);
      }
      const auto report =
          rotate_phi_keys(*backend, *kms, lab_filter, core::UserId::parse(actor), out);
      out << "rotated " << report.rewrapped << " of " << report.scanned
          << " PHI sample(s) (already current " << report.current << ", failed " << report.failed
          << ")\n";
      return report.failed == 0 ? 0 : 1;
    }

    // ---- backup noun (encrypted SQLite backup / restore / restore-drill) ----

    struct BackupSubcommands {
      std::string create_sqlite;
      std::string create_postgres;
      std::string create_out;
      std::string create_actor;
      CLI::App* create{nullptr};

      std::string verify_in;
      CLI::App* verify{nullptr};

      std::string restore_in;
      std::string restore_out;
      std::string restore_postgres;
      std::string restore_actor;
      bool restore_force{false};
      CLI::App* restore{nullptr};

      std::string run_sqlite;
      std::string run_postgres;
      std::string run_dir;
      std::string run_actor;
      int run_daily{30};
      int run_monthly{12};
      int run_yearly{7};
      double run_backup_interval_hours{24.0};
      double run_drill_interval_hours{168.0};
      CLI::App* run{nullptr};

      std::string list_dir;
      CLI::App* list{nullptr};
    };

    void add_backup_noun(CLI::App& app, BackupSubcommands& backup) {
      CLI::App* root =
          app.add_subcommand("backup", "Encrypted backup / restore (SQLite or PostgreSQL)");
      root->require_subcommand(1);

      backup.create = root->add_subcommand("create", "Back up + encrypt the live database");
      backup.create->add_option("--sqlite", backup.create_sqlite,
                                "Path to the live SQLite database");
      backup.create->add_option("--postgres", backup.create_postgres,
                                "PostgreSQL connection string (pg_dump source)");
      backup.create->add_option("--out", backup.create_out, "Encrypted backup file to write")
          ->required();
      backup.create->add_option("--actor", backup.create_actor, "User UUID recorded as the actor")
          ->required();

      backup.verify =
          root->add_subcommand("verify", "Restore-drill: check a backup decrypts and verifies");
      backup.verify->add_option("--in", backup.verify_in, "Encrypted backup file to verify")
          ->required();

      backup.restore = root->add_subcommand(
          "restore", "Decrypt a backup to a SQLite file or into a PostgreSQL database");
      backup.restore->add_option("--in", backup.restore_in, "Encrypted backup file")->required();
      backup.restore->add_option("--out", backup.restore_out,
                                 "SQLite database file to write (SQLite backups)");
      backup.restore->add_option("--postgres", backup.restore_postgres,
                                 "PostgreSQL connection string to restore into (Postgres backups)");
      backup.restore->add_flag("--force", backup.restore_force, "Overwrite --out if it exists");
      backup.restore->add_option("--actor", backup.restore_actor, "User UUID recorded as the actor")
          ->required();

      backup.run = root->add_subcommand(
          "run", "Scheduled tick: create-if-due, prune per retention, weekly restore drill");
      backup.run->add_option("--sqlite", backup.run_sqlite, "Path to the live SQLite database");
      backup.run->add_option("--postgres", backup.run_postgres,
                             "PostgreSQL connection string (pg_dump source)");
      backup.run->add_option("--dir", backup.run_dir, "Directory holding the encrypted backups")
          ->required();
      backup.run->add_option("--actor", backup.run_actor, "User UUID recorded as the actor")
          ->required();
      backup.run->add_option("--daily", backup.run_daily, "Daily backups to retain")
          ->capture_default_str();
      backup.run->add_option("--monthly", backup.run_monthly, "Monthly backups to retain")
          ->capture_default_str();
      backup.run->add_option("--yearly", backup.run_yearly, "Yearly backups to retain")
          ->capture_default_str();
      backup.run
          ->add_option("--backup-interval-hours", backup.run_backup_interval_hours,
                       "Create a new backup only if the newest is older than this")
          ->capture_default_str();
      backup.run
          ->add_option("--drill-interval-hours", backup.run_drill_interval_hours,
                       "Run a restore drill only if the last one is older than this")
          ->capture_default_str();

      backup.list = root->add_subcommand("list", "List backups in a directory (newest first)");
      backup.list->add_option("--dir", backup.list_dir, "Directory holding the encrypted backups")
          ->required();
    }

    // dispatch_backup is a flat dispatcher over five independent subcommands
    // (create/verify/restore/run/list); the cognitive-complexity score comes from
    // repeated guard boilerplate, not nested logic, so splitting it would not aid
    // readability.
    // NOLINTBEGIN(bugprone-easily-swappable-parameters)
    // NOLINTNEXTLINE(readability-function-cognitive-complexity)
    [[nodiscard]] std::optional<int> dispatch_backup(const BackupSubcommands& backup,
                                                     std::ostream& out, std::ostream& err) {
      // NOLINTEND(bugprone-easily-swappable-parameters)
      const auto load_backup_kms = [&err]() {
        auto provider = kms::make_backup_kms();
        if (provider == nullptr) {
          err << "error: no backup KEK configured (set CREDENTIALS_DIRECTORY/backup_kek "
                 "or FMGR_BACKUP_KEK)\n";
        }
        return provider;
      };

      if (backup.create->parsed()) {
        const bool has_pg = !backup.create_postgres.empty();
        if (has_pg == !backup.create_sqlite.empty()) {
          err << "error: backup create needs exactly one of --sqlite or --postgres\n";
          return 1;
        }
        auto provider = load_backup_kms();
        if (provider == nullptr) {
          return 1;
        }
        const auto actor = core::UserId::parse(backup.create_actor);
        if (has_pg) {
          auto backend = open_backend(backend_options_from("", backup.create_postgres));
          backup::run_backup_create_postgres(*backend, backup.create_postgres, *provider,
                                             backup.create_out, actor, out);
        } else {
          auto backend = open_backend(backend_options_from(backup.create_sqlite, ""));
          backup::run_backup_create(*backend, backup.create_sqlite, *provider, backup.create_out,
                                    actor, out);
        }
        return 0;
      }
      if (backup.verify->parsed()) {
        auto provider = load_backup_kms();
        if (provider == nullptr) {
          return 1;
        }
        return backup::run_backup_verify(backup.verify_in, *provider, out).ok ? 0 : 1;
      }
      if (backup.restore->parsed()) {
        const bool has_pg = !backup.restore_postgres.empty();
        if (has_pg == !backup.restore_out.empty()) {
          err << "error: backup restore needs exactly one of --out or --postgres\n";
          return 1;
        }
        auto provider = load_backup_kms();
        if (provider == nullptr) {
          return 1;
        }
        const auto actor = core::UserId::parse(backup.restore_actor);
        if (has_pg) {
          backup::run_backup_restore_postgres(backup.restore_in, *provider, backup.restore_postgres,
                                              actor, out);
        } else {
          backup::run_backup_restore(backup.restore_in, *provider, backup.restore_out,
                                     backup.restore_force, actor, out);
        }
        return 0;
      }
      if (backup.run->parsed()) {
        const bool has_pg = !backup.run_postgres.empty();
        if (has_pg == !backup.run_sqlite.empty()) {
          err << "error: backup run needs exactly one of --sqlite or --postgres\n";
          return 1;
        }
        auto provider = load_backup_kms();
        if (provider == nullptr) {
          return 1;
        }
        constexpr double k_micros_per_hour = 3'600.0 * 1'000'000.0;
        auto backend = open_backend(has_pg ? backend_options_from("", backup.run_postgres)
                                           : backend_options_from(backup.run_sqlite, ""));
        const backup::BackupScheduleConfig config{
            .sqlite_db_path = backup.run_sqlite,
            .postgres_url = backup.run_postgres,
            .backup_dir = backup.run_dir,
            .retention =
                backup::RetentionPolicy{backup.run_daily, backup.run_monthly, backup.run_yearly},
            .backup_interval_micros =
                static_cast<std::int64_t>(backup.run_backup_interval_hours * k_micros_per_hour),
            .drill_interval_micros =
                static_cast<std::int64_t>(backup.run_drill_interval_hours * k_micros_per_hour),
            .actor = core::UserId::parse(backup.run_actor),
        };
        return backup::run_backup_run(*backend, *provider, config, out);
      }
      if (backup.list->parsed()) {
        return backup::run_backup_list(backup.list_dir, out);
      }
      return std::nullopt;
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
      CLI::App* root{nullptr}; // shared parent so `create` can attach to it too
      CLI::App* list{nullptr};
      CLI::App* inspect{nullptr};
    };

    void add_read_noun(CLI::App& app, const std::string& name, NounSubcommands& noun) {
      CLI::App* root = app.add_subcommand(name, name + " commands");
      root->require_subcommand(1);
      noun.root = root;

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

    // ---- create nouns (write half of the M1 CLI nouns) ----

    [[nodiscard]] std::optional<std::string> opt_str(const std::string& text) {
      if (text.empty()) {
        return std::nullopt;
      }
      return text;
    }

    // Backend selector + the audit actor, shared by every create noun.
    struct CreateArgs {
      std::string sqlite;
      std::string postgres;
      std::string lab;
      std::string actor;
    };

    void add_create_common(CLI::App* sub, CreateArgs& args) {
      sub->add_option("--sqlite", args.sqlite, "Path to a SQLite database file");
      sub->add_option("--postgres", args.postgres, "PostgreSQL connection URL");
      sub->add_option("--lab", args.lab, "Lab UUID to create the entity in")->required();
      sub->add_option("--actor", args.actor, "User UUID recorded as the mutator")->required();
    }

    [[nodiscard]] CreateCommon to_create_common(const CreateArgs& args) {
      return CreateCommon{.lab_id = core::LabId::parse(args.lab),
                          .actor = core::UserId::parse(args.actor)};
    }

    // Parse-time storage for every create noun; CLI11 binds into these and the
    // typed option structs are built at dispatch time.
    struct CreateNouns {
      CreateArgs it_args;
      std::string it_name;
      std::string it_parent;
      CLI::App* item_type{nullptr};

      CreateArgs ct_args;
      std::string ct_name;
      std::string ct_size_class;
      std::string ct_material;
      std::string ct_sku;
      CLI::App* container_type{nullptr};

      CreateArgs sc_args;
      std::string sc_name;
      std::string sc_label;
      std::string sc_kind{"custom"};
      std::string sc_parent;
      std::int32_t sc_ordering{0};
      CLI::App* storage_container{nullptr};

      CreateArgs fz_args;
      std::string fz_name;
      std::string fz_location;
      std::string fz_model;
      std::optional<double> fz_temp;
      std::string fz_root;
      CLI::App* freezer{nullptr};

      CreateArgs bt_args;
      std::string bt_name;
      std::string bt_manufacturer;
      std::string bt_sku;
      std::string bt_accepts;
      std::int32_t bt_rows{0};
      std::int32_t bt_cols{0};
      CLI::App* box_type{nullptr};

      CreateArgs bx_args;
      std::string bx_label;
      std::string bx_box_type;
      std::string bx_container;
      std::string bx_serial;
      std::string bx_barcode;
      CLI::App* box{nullptr};
    };

    // Attach `create` under the existing freezer/box/item-type roots, and add new
    // roots for container-type/storage-container/box-type (no read noun yet). Pure
    // CLI11 wiring: long but flat.
    void add_create_nouns(CLI::App& app, const NounSubcommands& freezer, const NounSubcommands& box,
                          const NounSubcommands& item_type, CreateNouns& nouns) {
      nouns.item_type = item_type.root->add_subcommand("create", "Create an item-type in a lab");
      add_create_common(nouns.item_type, nouns.it_args);
      nouns.item_type->add_option("--name", nouns.it_name, "Item-type name")->required();
      nouns.item_type->add_option("--parent-id", nouns.it_parent,
                                  "Parent item-type UUID (omit for a root node)");

      CLI::App* container_type_root =
          app.add_subcommand("container-type", "container-type commands");
      container_type_root->require_subcommand(1);
      nouns.container_type =
          container_type_root->add_subcommand("create", "Create a container-type in a lab");
      add_create_common(nouns.container_type, nouns.ct_args);
      nouns.container_type->add_option("--name", nouns.ct_name, "Container-type name")->required();
      nouns.container_type->add_option("--size-class", nouns.ct_size_class, "Size-class token")
          ->required();
      nouns.container_type->add_option("--material", nouns.ct_material, "Material");
      nouns.container_type->add_option("--sku", nouns.ct_sku, "Supplier SKU");

      CLI::App* storage_container_root =
          app.add_subcommand("storage-container", "storage-container commands");
      storage_container_root->require_subcommand(1);
      nouns.storage_container =
          storage_container_root->add_subcommand("create", "Create a storage-container in a lab");
      add_create_common(nouns.storage_container, nouns.sc_args);
      nouns.storage_container->add_option("--name", nouns.sc_name, "Container name")->required();
      nouns.storage_container->add_option("--label", nouns.sc_label, "Display label");
      nouns.storage_container->add_option("--kind", nouns.sc_kind,
                                          "Kind: compartment|shelf|rack|drawer|custom");
      nouns.storage_container->add_option("--parent-id", nouns.sc_parent,
                                          "Parent storage-container UUID (omit for a root)");
      nouns.storage_container->add_option("--ordering-index", nouns.sc_ordering, "Ordering index");

      nouns.freezer = freezer.root->add_subcommand("create", "Create a freezer in a lab");
      add_create_common(nouns.freezer, nouns.fz_args);
      nouns.freezer->add_option("--name", nouns.fz_name, "Freezer name")->required();
      nouns.freezer
          ->add_option("--layout-root-id", nouns.fz_root,
                       "Root storage-container UUID (the freezer's top-level container)")
          ->required();
      nouns.freezer->add_option("--location", nouns.fz_location, "Physical location");
      nouns.freezer->add_option("--model", nouns.fz_model, "Model");
      nouns.freezer->add_option("--temp-c", nouns.fz_temp, "Target temperature in Celsius");

      CLI::App* box_type_root = app.add_subcommand("box-type", "box-type commands");
      box_type_root->require_subcommand(1);
      nouns.box_type =
          box_type_root->add_subcommand("create", "Create a box-type with a uniform position grid");
      add_create_common(nouns.box_type, nouns.bt_args);
      nouns.box_type->add_option("--name", nouns.bt_name, "Box-type name")->required();
      nouns.box_type->add_option("--rows", nouns.bt_rows, "Grid rows (1-26)")->required();
      nouns.box_type->add_option("--cols", nouns.bt_cols, "Grid columns")->required();
      nouns.box_type->add_option("--accepts", nouns.bt_accepts, "size_class every position accepts")
          ->required();
      nouns.box_type->add_option("--manufacturer", nouns.bt_manufacturer, "Manufacturer");
      nouns.box_type->add_option("--sku", nouns.bt_sku, "SKU");

      nouns.box = box.root->add_subcommand("create", "Create a box in a lab");
      add_create_common(nouns.box, nouns.bx_args);
      nouns.box->add_option("--label", nouns.bx_label, "Box label")->required();
      nouns.box->add_option("--box-type-id", nouns.bx_box_type, "BoxType UUID")->required();
      nouns.box->add_option("--container-id", nouns.bx_container, "Storage-container UUID")
          ->required();
      nouns.box->add_option("--serial", nouns.bx_serial, "Serial number");
      nouns.box->add_option("--barcode", nouns.bx_barcode, "Barcode");
    }

    [[nodiscard]] std::optional<int> dispatch_create_nouns_a(const CreateNouns& nouns,
                                                             std::ostream& out) {
      if (nouns.item_type->parsed()) {
        auto backend =
            open_backend(backend_options_from(nouns.it_args.sqlite, nouns.it_args.postgres));
        ItemTypeCreateOptions opts{.common = to_create_common(nouns.it_args),
                                   .name = nouns.it_name};
        if (!nouns.it_parent.empty()) {
          opts.parent_id = core::ItemTypeId::parse(nouns.it_parent);
        }
        run_item_type_create(*backend, opts, out);
        return 0;
      }
      if (nouns.container_type->parsed()) {
        auto backend =
            open_backend(backend_options_from(nouns.ct_args.sqlite, nouns.ct_args.postgres));
        run_container_type_create(
            *backend,
            ContainerTypeCreateOptions{.common = to_create_common(nouns.ct_args),
                                       .name = nouns.ct_name,
                                       .size_class = nouns.ct_size_class,
                                       .material = nouns.ct_material,
                                       .supplier_sku = nouns.ct_sku},
            out);
        return 0;
      }
      if (nouns.storage_container->parsed()) {
        auto backend =
            open_backend(backend_options_from(nouns.sc_args.sqlite, nouns.sc_args.postgres));
        StorageContainerCreateOptions opts{.common = to_create_common(nouns.sc_args),
                                           .name = nouns.sc_name,
                                           .label = nouns.sc_label,
                                           .kind = core::parse_container_kind(nouns.sc_kind),
                                           .ordering_index = nouns.sc_ordering};
        if (!nouns.sc_parent.empty()) {
          opts.parent_id = core::StorageContainerId::parse(nouns.sc_parent);
        }
        run_storage_container_create(*backend, opts, out);
        return 0;
      }
      return std::nullopt;
    }

    [[nodiscard]] std::optional<int> dispatch_create_nouns_b(const CreateNouns& nouns,
                                                             std::ostream& out) {
      if (nouns.freezer->parsed()) {
        auto backend =
            open_backend(backend_options_from(nouns.fz_args.sqlite, nouns.fz_args.postgres));
        run_freezer_create(
            *backend,
            FreezerCreateOptions{.common = to_create_common(nouns.fz_args),
                                 .name = nouns.fz_name,
                                 .location = nouns.fz_location,
                                 .model = nouns.fz_model,
                                 .temp_target_c = nouns.fz_temp,
                                 .layout_root_id = core::StorageContainerId::parse(nouns.fz_root)},
            out);
        return 0;
      }
      if (nouns.box_type->parsed()) {
        auto backend =
            open_backend(backend_options_from(nouns.bt_args.sqlite, nouns.bt_args.postgres));
        run_box_type_create(*backend,
                            BoxTypeCreateOptions{.common = to_create_common(nouns.bt_args),
                                                 .name = nouns.bt_name,
                                                 .manufacturer = nouns.bt_manufacturer,
                                                 .sku = nouns.bt_sku,
                                                 .rows = nouns.bt_rows,
                                                 .cols = nouns.bt_cols,
                                                 .accepts_size_class = nouns.bt_accepts},
                            out);
        return 0;
      }
      if (nouns.box->parsed()) {
        auto backend =
            open_backend(backend_options_from(nouns.bx_args.sqlite, nouns.bx_args.postgres));
        run_box_create(*backend,
                       BoxCreateOptions{.common = to_create_common(nouns.bx_args),
                                        .label = nouns.bx_label,
                                        .box_type_id = core::BoxTypeId::parse(nouns.bx_box_type),
                                        .storage_container_id =
                                            core::StorageContainerId::parse(nouns.bx_container),
                                        .serial = opt_str(nouns.bx_serial),
                                        .barcode = opt_str(nouns.bx_barcode)},
                       out);
        return 0;
      }
      return std::nullopt;
    }

    [[nodiscard]] std::optional<int> dispatch_create_nouns(const CreateNouns& nouns,
                                                           std::ostream& out) {
      if (const auto code = dispatch_create_nouns_a(nouns, out)) {
        return code;
      }
      return dispatch_create_nouns_b(nouns, out);
    }

    // ---- non-sample CSV import nouns (item-type / box) ----

    // Parse-time storage for one `<noun> import` subcommand.
    struct ImportArgs {
      std::string sqlite;
      std::string postgres;
      std::string lab;
      std::string actor;
      std::string file;
      bool dry_run{false};
      CLI::App* cmd{nullptr};
    };

    void add_import_noun(CLI::App* root, const std::string& noun, ImportArgs& args) {
      args.cmd = root->add_subcommand("import", "Import " + noun + "s from a CSV file");
      args.cmd->add_option("--sqlite", args.sqlite, "Path to a SQLite database file");
      args.cmd->add_option("--postgres", args.postgres, "PostgreSQL connection URL");
      args.cmd->add_option("--lab", args.lab, "Lab UUID to import into")->required();
      args.cmd->add_option("--actor", args.actor, "User UUID recorded as the importer")->required();
      args.cmd->add_flag("--dry-run", args.dry_run,
                         "Validate every row and report, but do not write anything");
      args.cmd->add_option("file", args.file, "CSV file to import ('-' for stdin)")->required();
    }

    // Parse-time storage for `user set-password`. The password is read from stdin
    // at dispatch, never taken on argv (it would leak via ps/shell history).
    struct SetPasswordArgs {
      std::string sqlite;
      std::string postgres;
      std::string email;
      std::string actor;
      CLI::App* cmd{nullptr};
    };

    void add_user_set_password(CLI::App* user_root, SetPasswordArgs& args) {
      args.cmd = user_root->add_subcommand(
          "set-password", "Enrol a local password for an existing user (reads it from stdin)");
      args.cmd->add_option("--sqlite", args.sqlite, "Path to a SQLite database file");
      args.cmd->add_option("--postgres", args.postgres, "PostgreSQL connection URL");
      args.cmd->add_option("--email", args.email, "Email of the user to enrol")->required();
      args.cmd->add_option("--actor", args.actor,
                           "User UUID recorded as the audit actor (default: the target user)");
    }

    struct ImportNouns {
      ImportArgs item_type;
      ImportArgs box;
      ImportArgs custom_field_def;
      ImportArgs user;
      SetPasswordArgs user_set_password;
    };

    void add_import_nouns(CLI::App& app, const NounSubcommands& box,
                          const NounSubcommands& item_type, ImportNouns& nouns) {
      add_import_noun(item_type.root, "item-type", nouns.item_type);
      add_import_noun(box.root, "box", nouns.box);

      // custom-field-def and user have no read/create noun yet → own roots.
      CLI::App* cfd_root = app.add_subcommand("custom-field-def", "custom-field-def commands");
      cfd_root->require_subcommand(1);
      add_import_noun(cfd_root, "custom-field-def", nouns.custom_field_def);

      CLI::App* user_root = app.add_subcommand("user", "user commands");
      user_root->require_subcommand(1);
      add_import_noun(user_root, "user", nouns.user);
      add_user_set_password(user_root, nouns.user_set_password);
    }

    // Run one parsed import: open the backend, then stream the file (or stdin on
    // '-') through the templated transactional runner with the entity's mapper. A
    // missing file throws; run_cli's catch reports it on the error stream.
    template <typename T, typename BuildFn>
    [[nodiscard]] int run_import_arg(const ImportArgs& args, BuildFn build_fn,
                                     const char* noun_plural, std::ostream& out) {
      auto backend = open_backend(backend_options_from(args.sqlite, args.postgres));
      const EntityImportOptions options{.lab_id = core::LabId::parse(args.lab),
                                        .actor = core::UserId::parse(args.actor),
                                        .dry_run = args.dry_run};
      if (args.file == "-") {
        return run_entity_import<T>(*backend, options, build_fn, std::cin, out, noun_plural);
      }
      std::ifstream file(args.file, std::ios::binary);
      if (!file) {
        throw std::runtime_error("cannot open input file: " + args.file);
      }
      return run_entity_import<T>(*backend, options, build_fn, file, out, noun_plural);
    }

    [[nodiscard]] std::optional<int> dispatch_import_nouns(const ImportNouns& nouns,
                                                           std::ostream& out) {
      if (nouns.item_type.cmd->parsed()) {
        return run_import_arg<core::ItemType>(nouns.item_type, build_item_type_import,
                                              "item-type(s)", out);
      }
      if (nouns.box.cmd->parsed()) {
        return run_import_arg<core::Box>(nouns.box, build_box_import, "box(es)", out);
      }
      if (nouns.custom_field_def.cmd->parsed()) {
        return run_import_arg<core::CustomFieldDefinition>(
            nouns.custom_field_def, build_custom_field_def_import, "custom-field-def(s)", out);
      }
      if (nouns.user.cmd->parsed()) {
        return run_import_arg<core::User>(nouns.user, build_user_import, "user(s)", out);
      }
      return std::nullopt;
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

    // audit export: chain-of-custody CSV of the audit log. The export is itself
    // audited (PRD §7.3), so it needs an --actor; --lab scopes it to one lab.
    std::string audit_export_sqlite;
    std::string audit_export_postgres;
    std::string audit_export_lab;   // optional; empty = all labs (global export)
    std::string audit_export_actor; // required
    std::string audit_export_out;   // optional; empty = stdout
    CLI::App* audit_export =
        audit->add_subcommand("export", "Export the audit log as chain-of-custody CSV");
    audit_export->add_option("--sqlite", audit_export_sqlite, "Path to a SQLite database file");
    audit_export->add_option("--postgres", audit_export_postgres, "PostgreSQL connection URL");
    audit_export->add_option("--lab", audit_export_lab, "Lab UUID to scope the export to");
    audit_export->add_option("--actor", audit_export_actor, "User UUID recorded as the exporter")
        ->required();
    audit_export->add_option("--out", audit_export_out, "Write CSV to this file instead of stdout");

    // lab create: first-run bootstrap. No --lab (it mints one); ids are
    // server-generated. Takes its own backend selector + lab/admin descriptors.
    std::string lab_sqlite;
    std::string lab_postgres;
    LabCreateOptions lab_create_opts;
    CLI::App* lab = app.add_subcommand("lab", "Lab provisioning commands");
    lab->require_subcommand(1);
    CLI::App* lab_create =
        lab->add_subcommand("create", "Provision a new lab and its first SystemAdmin user");
    lab_create->add_option("--sqlite", lab_sqlite, "Path to a SQLite database file");
    lab_create->add_option("--postgres", lab_postgres, "PostgreSQL connection URL");
    lab_create->add_option("--name", lab_create_opts.name, "Lab name")->required();
    lab_create->add_option("--contact", lab_create_opts.contact, "Lab contact (email or phone)");
    lab_create->add_option("--admin-email", lab_create_opts.admin_email, "First SystemAdmin email")
        ->required();
    lab_create->add_option("--admin-name", lab_create_opts.admin_display_name,
                           "First SystemAdmin display name");
    lab_create->add_flag("--phi", lab_create_opts.phi_enabled, "Enable PHI mode for this lab");

    // key rotate: re-wrap PHI envelopes under the active master KEK (PRD §8).
    std::string key_sqlite;
    std::string key_postgres;
    std::string key_lab;   // optional; empty = all labs
    std::string key_actor; // required
    CLI::App* key = app.add_subcommand("key", "Encryption key commands");
    key->require_subcommand(1);
    CLI::App* key_rotate =
        key->add_subcommand("rotate", "Re-wrap PHI envelopes under the active master KEK");
    key_rotate->add_option("--sqlite", key_sqlite, "Path to a SQLite database file");
    key_rotate->add_option("--postgres", key_postgres, "PostgreSQL connection URL");
    key_rotate->add_option("--lab", key_lab, "Lab UUID to rotate (default: all labs)");
    key_rotate->add_option("--actor", key_actor, "User UUID recorded as the rotation actor")
        ->required();

    NounSubcommands freezer_noun;
    NounSubcommands box_noun;
    NounSubcommands item_type_noun;
    add_read_noun(app, "freezer", freezer_noun);
    add_read_noun(app, "box", box_noun);
    add_read_noun(app, "item-type", item_type_noun);

    CreateNouns create_nouns;
    add_create_nouns(app, freezer_noun, box_noun, item_type_noun, create_nouns);

    ImportNouns import_nouns;
    add_import_nouns(app, box_noun, item_type_noun, import_nouns);

    BackupSubcommands backup_noun;
    add_backup_noun(app, backup_noun);

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
        auto backend = open_backend(backend_options_from(import_sqlite, import_postgres));
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
        auto backend = open_backend(backend_options_from(audit_sqlite, audit_postgres));
        return run_audit_verify(*backend, AuditVerifyOptions{}, out);
      }
      if (audit_export->parsed()) {
        auto backend =
            open_backend(backend_options_from(audit_export_sqlite, audit_export_postgres));
        const AuditExportOptions export_options{
            .lab_id = audit_export_lab.empty()
                          ? std::nullopt
                          : std::optional<core::LabId>(core::LabId::parse(audit_export_lab)),
            .actor = core::UserId::parse(audit_export_actor),
        };
        if (audit_export_out.empty()) {
          return run_audit_export(*backend, export_options, out);
        }
        std::ofstream file(audit_export_out, std::ios::binary | std::ios::trunc);
        if (!file) {
          err << "error: cannot open output file: " << audit_export_out << '\n';
          return 1;
        }
        return run_audit_export(*backend, export_options, file);
      }
      if (lab_create->parsed()) {
        auto backend = open_backend(backend_options_from(lab_sqlite, lab_postgres));
        run_lab_create(*backend, lab_create_opts, out);
        return 0;
      }
      if (const auto code = dispatch_key_rotate(key_rotate->parsed(), key_sqlite, key_postgres,
                                                key_lab, key_actor, out, err)) {
        return *code;
      }
      if (const auto code = dispatch_read_nouns(freezer_noun, box_noun, item_type_noun, out)) {
        return *code;
      }
      if (const auto code = dispatch_create_nouns(create_nouns, out)) {
        return *code;
      }
      if (const auto code = dispatch_import_nouns(import_nouns, out)) {
        return *code;
      }
      if (import_nouns.user_set_password.cmd->parsed()) {
        const auto& args = import_nouns.user_set_password;
        auto backend = open_backend(backend_options_from(args.sqlite, args.postgres));
        // Read the password from stdin (one line); never accept it on argv.
        std::string password;
        std::getline(std::cin, password);
        const std::optional<core::UserId> actor =
            args.actor.empty() ? std::nullopt
                               : std::optional<core::UserId>(core::UserId::parse(args.actor));
        return run_user_set_password(*backend, args.email, password, actor, out, err);
      }
      if (const auto code = dispatch_backup(backup_noun, out, err)) {
        return *code;
      }
    } catch (const std::exception& error) {
      err << "error: " << error.what() << '\n';
      return 1;
    }

    return 0;
  }

} // namespace fmgr::cli
