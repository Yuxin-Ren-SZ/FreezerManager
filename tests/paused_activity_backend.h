// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FMGR_TESTS_PAUSED_ACTIVITY_BACKEND_H
#define FMGR_TESTS_PAUSED_ACTIVITY_BACKEND_H

#include "storage/IStorageBackend.h"

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <vector>

namespace fmgr::test {

  // Stop the next write before it owns DB resources. Reads preceding it have
  // completed, so another provider can commit a controlled competing mutation.
  // Every wait is bounded, and release is idempotent for assertion cleanup.
  class PausedActivityBackend final : public storage::IStorageBackend {
  public:
    explicit PausedActivityBackend(storage::IStorageBackend& backend) : backend_(backend) {}

    ~PausedActivityBackend() override {
      release();
    }

    void migrate_to_latest() override {
      backend_.migrate_to_latest();
    }

    [[nodiscard]] storage::SchemaVersion current_version() const override {
      return backend_.current_version();
    }

    [[nodiscard]] std::unique_ptr<storage::ITransaction>
    begin(storage::IsolationLevel isolation) override {
      if (isolation == storage::IsolationLevel::Serializable) {
        pause_before_write();
      }
      return backend_.begin(isolation);
    }

    [[nodiscard]] std::unique_ptr<storage::ITransaction>
    begin_auth_transaction(const std::vector<core::UserId>& user_ids) override {
      pause_before_write();
      return backend_.begin_auth_transaction(user_ids);
    }

    [[nodiscard]] storage::Capabilities caps() const override {
      return backend_.caps();
    }

    [[nodiscard]] bool wait_until_paused() {
      return reached_future_.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    }

    void release() {
      if (!released_.exchange(true)) {
        resume_.set_value();
      }
    }

  private:
    void pause_before_write() {
      if (armed_.exchange(false)) {
        reached_.set_value();
        if (resume_future_.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
          throw storage::Unavailable("timed out waiting for activity test release");
        }
      }
    }

    storage::IStorageBackend& backend_;
    std::atomic<bool> armed_{true};
    std::atomic<bool> released_{false};
    std::promise<void> reached_;
    std::future<void> reached_future_{reached_.get_future()};
    std::promise<void> resume_;
    std::future<void> resume_future_{resume_.get_future()};
  };

} // namespace fmgr::test
#endif // FMGR_TESTS_PAUSED_ACTIVITY_BACKEND_H
