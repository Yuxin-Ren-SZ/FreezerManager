// SPDX-License-Identifier: AGPL-3.0-or-later

#include "server/BackupScheduler.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdint>
#include <exception>
#include <random>
#include <sstream>
#include <utility>

namespace fmgr::server {

  namespace {

    std::int64_t now_micros() {
      return std::chrono::duration_cast<std::chrono::microseconds>(
                 std::chrono::system_clock::now().time_since_epoch())
          .count();
    }

  } // namespace

  BackupScheduler::BackupScheduler(storage::IStorageBackend& backend,
                                   std::unique_ptr<kms::IKmsProvider> backup_kms,
                                   backup::BackupScheduleConfig config,
                                   std::chrono::milliseconds poll_interval)
      : backend_(backend), backup_kms_(std::move(backup_kms)), config_(std::move(config)),
        poll_interval_(poll_interval) {}

  BackupScheduler::~BackupScheduler() {
    stop();
  }

  void BackupScheduler::start() {
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (running_) {
        return;
      }
      running_ = true;
    }
    thread_ = std::thread([this] { loop(); });
  }

  void BackupScheduler::stop() {
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (!running_) {
        return;
      }
      running_ = false;
    }
    cv_.notify_all();
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  void BackupScheduler::loop() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (running_) {
      lock.unlock();
      run_one_tick();
      lock.lock();
      // Wake early if stop() flips running_ during the wait.
      cv_.wait_for(lock, poll_interval_, [this] { return !running_; });
    }
  }

  void BackupScheduler::run_one_tick() {
    std::random_device device;
    const auto seed =
        (static_cast<std::uint64_t>(device()) << 32U) ^ static_cast<std::uint64_t>(device());
    std::ostringstream log;
    try {
      const auto result =
          backup::run_backup_tick(backend_, *backup_kms_, config_, now_micros(), seed, log);
      if (result.backup_made) {
        spdlog::info("backup scheduler: wrote {}", result.backup_path);
      }
      if (result.pruned > 0) {
        spdlog::info("backup scheduler: pruned {} old backup(s)", result.pruned);
      }
      if (result.drill_ran && !result.drill_ok) {
        // Page the system admin: a stored backup failed its restore drill.
        spdlog::error("backup scheduler: RESTORE DRILL FAILED for {} — {}", result.drill_target,
                      log.str());
      } else if (result.drill_ran) {
        spdlog::info("backup scheduler: restore drill passed for {}", result.drill_target);
      }
    } catch (const std::exception& error) {
      spdlog::error("backup scheduler: tick failed: {} ({})", error.what(), log.str());
    }
  }

} // namespace fmgr::server
