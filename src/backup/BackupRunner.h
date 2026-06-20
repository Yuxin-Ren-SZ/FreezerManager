// SPDX-License-Identifier: AGPL-3.0-or-later

// BackupRunner — one scheduled-backup "tick" (PRD §14): create a fresh encrypted
// backup when one is due, prune old backups per the GFS RetentionPolicy, and run
// a restore drill on a randomly chosen backup when the weekly cadence is due.
//
// All time and randomness are injected (`now_micros`, `rng_seed`) so the tick is
// deterministic and unit-testable; the in-server BackupScheduler and the
// `freezerctl backup run` CLI both drive it, the former on a real clock loop, the
// latter once per invocation. The tick reuses the primitive engines in
// BackupCommands (run_backup_create / run_backup_verify) so backup behavior is
// defined in exactly one place.
//
// Cadence state lives in the backup directory itself: a backup is due when the
// newest `fmgr-*.fmgrbak` file (timestamp read from its name, no decryption) is
// older than `backup_interval_micros`; a drill is due when a `.last_drill` marker
// file is older than `drill_interval_micros` (or absent).
#ifndef FMGR_BACKUP_BACKUPRUNNER_H
#define FMGR_BACKUP_BACKUPRUNNER_H

#include "backup/RetentionPolicy.h"
#include "core/ids.h"
#include "kms/IKmsProvider.h"
#include "storage/IStorageBackend.h"

#include <cstdint>
#include <ostream>
#include <string>

namespace fmgr::backup {

  struct BackupScheduleConfig {
    std::string sqlite_db_path; // live database to hot-copy
    std::string backup_dir;     // directory holding fmgr-*.fmgrbak files
    RetentionPolicy retention{};
    std::int64_t backup_interval_micros{}; // create cadence (e.g. nightly)
    std::int64_t drill_interval_micros{};  // restore-drill cadence (e.g. weekly)
    core::UserId actor{};                  // recorded as the audit actor
  };

  struct TickResult {
    bool backup_made{};
    std::string backup_path;
    int pruned{};
    bool drill_ran{};
    bool drill_ok{};
    std::string drill_target; // filename of the backup the drill verified
  };

  // Run one tick against an injected clock and RNG seed. `backend` is the live
  // backend whose audit chain records backup.create / backup.prune / backup.drill
  // events. Progress is written to `log`. Throws BackupError / CipherError only on
  // an unexpected failure of the create path; a *bad backup* found by the drill is
  // reported via TickResult.drill_ok=false, never thrown (schedule-safe).
  [[nodiscard]] TickResult run_backup_tick(storage::IStorageBackend& backend,
                                           const kms::IKmsProvider& backup_kms,
                                           const BackupScheduleConfig& config,
                                           std::int64_t now_micros, std::uint64_t rng_seed,
                                           std::ostream& log);

  // CLI `freezerctl backup run`: one tick on the real clock with a fresh random
  // seed. Returns 0 normally, 1 if a drill ran and the backup failed verification
  // (so cron/systemd surfaces the failure).
  [[nodiscard]] int run_backup_run(storage::IStorageBackend& backend,
                                   const kms::IKmsProvider& backup_kms,
                                   const BackupScheduleConfig& config, std::ostream& out);

  // CLI `freezerctl backup list`: print the backups in `backup_dir` (newest
  // first) as `filename  created_at(UTC)  size_bytes`, without decrypting them.
  // Returns 0. Files not matching the naming convention are ignored.
  [[nodiscard]] int run_backup_list(const std::string& backup_dir, std::ostream& out);

} // namespace fmgr::backup

#endif // FMGR_BACKUP_BACKUPRUNNER_H
