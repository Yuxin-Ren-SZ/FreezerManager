// SPDX-License-Identifier: AGPL-3.0-or-later

// Property tests for the GFS backup-retention selector. The data-safety
// invariants are: deletion is a strict partition of the input (keep ∪ prune ==
// input, keep ∩ prune == ∅), nothing outside the input is ever returned, and the
// single newest backup is never pruned.

#include "backup/RetentionPolicy.h"

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include <gtest/gtest.h>
#include <rapidcheck.h>

namespace fmgr::backup {
  namespace {

    constexpr std::int64_t kMicros = 1'000'000;
    constexpr std::int64_t kQuarterDay = (86'400 * kMicros) / 4;

    TEST(RetentionPolicyProperty, DeletionPartitionsInput) {
      const bool passed = rc::check("deletion is a subset partition keeping the newest", [] {
        // Random backups, each tagged uniquely so set membership is unambiguous.
        const auto quarter_days =
            *rc::gen::container<std::vector<std::int64_t>>(rc::gen::inRange<std::int64_t>(0, 4000));
        std::vector<BackupFile> files;
        files.reserve(quarter_days.size());
        for (std::size_t i = 0; i < quarter_days.size(); ++i) {
          files.push_back(BackupFile{.path = "b" + std::to_string(i),
                                     .created_micros = quarter_days[i] * kQuarterDay});
        }
        const RetentionPolicy policy{*rc::gen::inRange(0, 40), *rc::gen::inRange(0, 20),
                                     *rc::gen::inRange(0, 12)};
        const std::int64_t now = 5000 * kQuarterDay;

        const auto deleted = select_for_deletion(files, policy, now);

        std::unordered_set<std::string> input_tags;
        for (const auto& file : files) {
          input_tags.insert(file.path);
        }
        std::unordered_set<std::string> deleted_tags;
        for (const auto& file : deleted) {
          RC_ASSERT(input_tags.count(file.path) == 1);      // every deletion came from the input
          RC_ASSERT(deleted_tags.insert(file.path).second); // and only once
        }
        RC_ASSERT(deleted.size() <= files.size());

        if (!files.empty()) {
          std::size_t newest = 0;
          for (std::size_t i = 1; i < files.size(); ++i) {
            if (files[i].created_micros > files[newest].created_micros) {
              newest = i;
            }
          }
          RC_ASSERT(deleted_tags.count(files[newest].path) == 0); // newest is never pruned
        }
      });
      EXPECT_TRUE(passed);
    }

  } // namespace
} // namespace fmgr::backup
