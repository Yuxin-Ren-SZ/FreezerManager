// SPDX-License-Identifier: AGPL-3.0-or-later

#include "backup/BackupRunner.h"

#include "backup/BackupCommands.h"
#include "backup/BackupFilename.h"
#include "backup/RetentionPolicy.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <string>
#include <system_error>
#include <vector>

namespace fmgr::backup {

  namespace {

    namespace fs = std::filesystem;

    constexpr std::int64_t k_micros_per_second = 1'000'000;
    constexpr const char* k_drill_marker = ".last_drill";

    std::int64_t now_micros() {
      return std::chrono::duration_cast<std::chrono::microseconds>(
                 std::chrono::system_clock::now().time_since_epoch())
          .count();
    }

    // Enumerate the directory's backups (filename-parsed timestamps only). Sorted
    // newest first.
    std::vector<BackupFile> list_backups(const std::string& dir) {
      std::vector<BackupFile> files;
      std::error_code error;
      for (const auto& entry : fs::directory_iterator(dir, error)) {
        if (error) {
          break;
        }
        if (!entry.is_regular_file(error)) {
          continue;
        }
        const std::string name = entry.path().filename().string();
        const auto micros = parse_backup_timestamp(name);
        if (micros.has_value()) {
          files.push_back(BackupFile{.path = entry.path().string(), .created_micros = *micros});
        }
      }
      std::sort(files.begin(), files.end(), [](const BackupFile& a, const BackupFile& b) {
        return a.created_micros > b.created_micros;
      });
      return files;
    }

    std::optional<std::int64_t> read_drill_marker(const std::string& dir) {
      const fs::path marker = fs::path(dir) / k_drill_marker;
      std::ifstream marker_in(marker);
      std::int64_t value = 0;
      if (marker_in >> value) {
        return value;
      }
      return std::nullopt;
    }

    void write_drill_marker(const std::string& dir, std::int64_t micros) {
      const fs::path marker = fs::path(dir) / k_drill_marker;
      std::ofstream out(marker, std::ios::trunc);
      out << micros << '\n';
    }

  } // namespace

  // NOLINTBEGIN(bugprone-easily-swappable-parameters)
  TickResult run_backup_tick(storage::IStorageBackend& backend, const kms::IKmsProvider& backup_kms,
                             const BackupScheduleConfig& config, std::int64_t now_micros_value,
                             std::uint64_t rng_seed, std::ostream& log) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    TickResult result;
    std::error_code error;
    fs::create_directories(config.backup_dir, error);

    std::vector<BackupFile> backups = list_backups(config.backup_dir);

    // ---- create, if due -------------------------------------------------------
    const bool backup_due =
        backups.empty() ||
        (now_micros_value - backups.front().created_micros) >= config.backup_interval_micros;
    if (backup_due) {
      const std::string out_path =
          (fs::path(config.backup_dir) / make_backup_filename(now_micros_value)).string();
      const auto report = config.postgres_url.empty()
                              ? run_backup_create(backend, config.sqlite_db_path, backup_kms,
                                                  out_path, config.actor, log)
                              : run_backup_create_postgres(backend, config.postgres_url, backup_kms,
                                                           out_path, config.actor, log);
      result.backup_made = true;
      result.backup_path = report.out_path;
      backups = list_backups(config.backup_dir); // include the new file in pruning
    } else {
      log << "backup not due (newest is "
          << (now_micros_value - backups.front().created_micros) / k_micros_per_second
          << "s old)\n";
    }

    // ---- prune per retention policy ------------------------------------------
    for (const auto& victim : select_for_deletion(backups, config.retention, now_micros_value)) {
      std::error_code remove_error;
      fs::remove(victim.path, remove_error);
      if (remove_error) {
        log << "warning: cannot remove " << victim.path << ": " << remove_error.message() << '\n';
        continue;
      }
      const std::string filename = fs::path(victim.path).filename().string();
      append_backup_event(
          backend, "backup.prune", filename,
          nlohmann::json{{"path", victim.path}, {"created_at_micros", victim.created_micros}},
          config.actor, log);
      ++result.pruned;
      log << "pruned " << filename << '\n';
    }

    // ---- restore drill, if due -----------------------------------------------
    const auto last_drill = read_drill_marker(config.backup_dir);
    const bool drill_due =
        !last_drill.has_value() || (now_micros_value - *last_drill) >= config.drill_interval_micros;
    const std::vector<BackupFile> remaining = list_backups(config.backup_dir);
    if (drill_due && !remaining.empty()) {
      std::mt19937_64 rng(rng_seed);
      std::uniform_int_distribution<std::size_t> pick(0, remaining.size() - 1);
      const BackupFile& target = remaining[pick(rng)];
      result.drill_target = fs::path(target.path).filename().string();

      const auto verify = run_backup_verify(target.path, backup_kms, log);
      result.drill_ran = true;
      result.drill_ok = verify.ok;
      append_backup_event(backend, "backup.drill", result.drill_target,
                          nlohmann::json{{"path", target.path}, {"ok", verify.ok}}, config.actor,
                          log);
      write_drill_marker(config.backup_dir, now_micros_value);
      if (!verify.ok) {
        log << "DRILL FAILED for " << result.drill_target << ": " << verify.detail << '\n';
      }
    }

    return result;
  }

  int run_backup_run(storage::IStorageBackend& backend, const kms::IKmsProvider& backup_kms,
                     const BackupScheduleConfig& config, std::ostream& out) {
    std::random_device device;
    const auto seed =
        (static_cast<std::uint64_t>(device()) << 32U) ^ static_cast<std::uint64_t>(device());
    const auto result = run_backup_tick(backend, backup_kms, config, now_micros(), seed, out);
    return (result.drill_ran && !result.drill_ok) ? 1 : 0;
  }

  int run_backup_list(const std::string& backup_dir, std::ostream& out) {
    const auto backups = list_backups(backup_dir);
    if (backups.empty()) {
      out << "no backups in " << backup_dir << '\n';
      return 0;
    }
    for (const auto& backup : backups) {
      std::error_code error;
      const auto size = std::filesystem::file_size(backup.path, error);
      const std::string filename = std::filesystem::path(backup.path).filename().string();
      out << filename << "  " << (error ? 0 : size) << " bytes\n";
    }
    return 0;
  }

} // namespace fmgr::backup
