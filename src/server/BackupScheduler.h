// SPDX-License-Identifier: AGPL-3.0-or-later

// BackupScheduler — the in-process scheduled-backup runner (PRD §14). A single
// background thread wakes on a fixed poll interval and drives one
// backup::run_backup_tick: create a backup when one is due, prune per the GFS
// retention policy, and run a weekly restore drill. Due-ness is decided inside
// the tick from the backup directory's contents, so the poll interval only sets
// how promptly a due job is noticed, not the backup cadence itself.
//
// A failing tick (or a failed restore drill) is logged via spdlog at error level
// — the "page the system admin" hook — and never tears the thread down, so a
// transient I/O error does not silently stop future backups.
#ifndef FMGR_SERVER_BACKUPSCHEDULER_H
#define FMGR_SERVER_BACKUPSCHEDULER_H

#include "backup/BackupRunner.h"
#include "kms/IKmsProvider.h"
#include "storage/IStorageBackend.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace fmgr::server {

  class BackupScheduler {
  public:
    // `backup_kms` is the separate backup KEK provider; the scheduler takes
    // ownership. `poll_interval` is how often the thread re-checks due-ness.
    BackupScheduler(storage::IStorageBackend& backend,
                    std::unique_ptr<kms::IKmsProvider> backup_kms,
                    backup::BackupScheduleConfig config,
                    std::chrono::milliseconds poll_interval = std::chrono::seconds(60));
    ~BackupScheduler();

    BackupScheduler(const BackupScheduler&) = delete;
    BackupScheduler& operator=(const BackupScheduler&) = delete;

    // Launch the background thread (runs one tick immediately, then every
    // poll_interval). Idempotent: a second call while running is a no-op.
    void start();

    // Signal the thread to stop and join it. Idempotent; also called by the dtor.
    void stop();

  private:
    void loop();
    void run_one_tick();

    storage::IStorageBackend& backend_;
    std::unique_ptr<kms::IKmsProvider> backup_kms_;
    backup::BackupScheduleConfig config_;
    std::chrono::milliseconds poll_interval_;

    std::mutex mutex_;
    std::condition_variable cv_;
    bool running_{false};
    std::thread thread_;
  };

} // namespace fmgr::server

#endif // FMGR_SERVER_BACKUPSCHEDULER_H
