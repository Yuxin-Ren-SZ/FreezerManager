// SPDX-License-Identifier: AGPL-3.0-or-later

#include "backup/RetentionPolicy.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace fmgr::backup {
  namespace {

    constexpr std::int64_t kMicros = 1'000'000;
    constexpr std::int64_t kDay = 86'400 * kMicros;
    constexpr std::int64_t kHour = 3'600 * kMicros;
    constexpr std::int64_t kNever = 1'000'000'000'000'000; // far future "now"

    BackupFile at(std::int64_t micros, const std::string& tag) {
      return BackupFile{.path = tag, .created_micros = micros};
    }

    std::vector<std::string> deleted_tags(const std::vector<BackupFile>& files,
                                          const RetentionPolicy& policy, std::int64_t now) {
      std::vector<std::string> tags;
      for (const auto& f : select_for_deletion(files, policy, now)) {
        tags.push_back(f.path);
      }
      std::sort(tags.begin(), tags.end());
      return tags;
    }

    TEST(RetentionPolicyTest, EmptyDeletesNothing) {
      EXPECT_TRUE(select_for_deletion({}, RetentionPolicy{}, kNever).empty());
    }

    TEST(RetentionPolicyTest, SingleBackupAlwaysKept) {
      const std::vector<BackupFile> files{at(5 * kDay, "only")};
      EXPECT_TRUE(select_for_deletion(files, RetentionPolicy{0, 0, 0}, kNever).empty());
    }

    TEST(RetentionPolicyTest, DailyTierKeepsNewestDays) {
      std::vector<BackupFile> files;
      for (int d = 0; d < 5; ++d) {
        files.push_back(at(d * kDay, "d" + std::to_string(d)));
      }
      // Keep the 3 most-recent days (d4,d3,d2); prune d1,d0.
      EXPECT_EQ(deleted_tags(files, RetentionPolicy{3, 0, 0}, kNever),
                (std::vector<std::string>{"d0", "d1"}));
    }

    TEST(RetentionPolicyTest, CollapsesMultipleBackupsInSameDay) {
      const std::vector<BackupFile> files{
          at(5 * kDay, "morning"),
          at(5 * kDay + 5 * kHour, "evening"),
          at(6 * kDay, "next"),
      };
      // Within a day only the newest survives; "morning" is pruned.
      EXPECT_EQ(deleted_tags(files, RetentionPolicy{10, 0, 0}, kNever),
                (std::vector<std::string>{"morning"}));
    }

    TEST(RetentionPolicyTest, MonthlyTierKeepsNewestMonths) {
      // ~40-day spacing lands each backup in a distinct calendar month.
      const std::vector<BackupFile> files{
          at(0 * kDay, "jan"),   // 1970-01-01
          at(40 * kDay, "feb"),  // 1970-02-10
          at(80 * kDay, "mar"),  // 1970-03-22
          at(120 * kDay, "may"), // 1970-05-01
      };
      // daily=0 disables the day tier; keep the 2 newest months (may, mar).
      EXPECT_EQ(deleted_tags(files, RetentionPolicy{0, 2, 0}, kNever),
                (std::vector<std::string>{"feb", "jan"}));
    }

    TEST(RetentionPolicyTest, ZeroPolicyKeepsOnlyNewest) {
      const std::vector<BackupFile> files{
          at(0 * kDay, "old"),
          at(1 * kDay, "mid"),
          at(2 * kDay, "new"),
      };
      EXPECT_EQ(deleted_tags(files, RetentionPolicy{0, 0, 0}, kNever),
                (std::vector<std::string>{"mid", "old"}));
    }

    TEST(RetentionPolicyTest, FutureDatedBackupsAlwaysKept) {
      const std::int64_t now = 2 * kDay;
      const std::vector<BackupFile> files{
          at(0 * kDay, "d0"), at(1 * kDay, "d1"), at(3 * kDay, "future_a"), // after now
          at(5 * kDay, "future_b"),                                         // after now
      };
      const auto tags = deleted_tags(files, RetentionPolicy{1, 0, 0}, now);
      EXPECT_EQ(std::find(tags.begin(), tags.end(), "future_a"), tags.end());
      EXPECT_EQ(std::find(tags.begin(), tags.end(), "future_b"), tags.end());
    }

  } // namespace
} // namespace fmgr::backup
