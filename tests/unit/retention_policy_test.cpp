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

    // ---- edge cases ----

    TEST(RetentionPolicyTest, DuplicateTimestampsBrokenByPosition) {
      // Two files with the same timestamp; the one earlier in the input order
      // (index 0) is kept as the "newest" in its bucket, the later one pruned.
      const std::vector<BackupFile> files{
          at(2 * kDay, "a"), // kept (first seen in this bucket)
          at(2 * kDay, "b"), // pruned (same bucket, later position)
      };
      EXPECT_EQ(deleted_tags(files, RetentionPolicy{1, 0, 0}, kNever),
                (std::vector<std::string>{"b"}));
    }

    TEST(RetentionPolicyTest, NegativeCountsTreatedAsZero) {
      // A negative policy count should not keep anything beyond the safety
      // floor (newest overall).  If it crashes or keeps more, that's a bug.
      const std::vector<BackupFile> files{
          at(0 * kDay, "old"),
          at(1 * kDay, "mid"),
          at(2 * kDay, "new"),
      };
      const auto tags = deleted_tags(files, RetentionPolicy{-5, -3, -1}, kNever);
      // Only the newest overall is kept.
      EXPECT_EQ(tags, (std::vector<std::string>{"mid", "old"}));
    }

    TEST(RetentionPolicyTest, YearlyTierKeepsNewestYears) {
      // ~400-day spacing lands each backup in a distinct calendar year.
      const std::vector<BackupFile> files{
          at(0 * kDay, "y1970"),    // 1970-01-01
          at(400 * kDay, "y1971"),  // 1971-02-04
          at(800 * kDay, "y1972"),  // 1972-03-12
          at(1200 * kDay, "y1974"), // 1974-04-18
      };
      // Keep 2 newest years (y1974, y1972); prune y1971, y1970.
      EXPECT_EQ(deleted_tags(files, RetentionPolicy{0, 0, 2}, kNever),
                (std::vector<std::string>{"y1970", "y1971"}));
    }

    TEST(RetentionPolicyTest, AllTiersActive) {
      // 6 backups over 6 days across 3 months — daily=3 keeps 3 days,
      // monthly=2 keeps 2 months, yearly keeps all (only 1 year spanned).
      const std::vector<BackupFile> files{
          at(0 * kDay, "day0"),    // d0, m0, y0 — pruned (day-tier exceeded)
          at(1 * kDay, "day1"),    // d1, m0, y0 — pruned
          at(2 * kDay, "day2"),    // d2, m0, y0 — kept (daily=3)
          at(3 * kDay, "day3"),    // d3, m0, y0 — kept
          at(33 * kDay, "month1"), // d33, m1, y0 — kept (monthly=2)
          at(63 * kDay, "month2"), // d63, m2, y0 — kept (monthly=2, daily=3)
      };
      const auto tags = deleted_tags(files, RetentionPolicy{3, 2, 10}, kNever);
      // Daily=3 keeps 3 newest day-buckets (63, 33, 3 → month2, month1, day3).
      // Monthly/yearly add nothing beyond that.  day2/day1/day0 pruned.
      EXPECT_EQ(tags, (std::vector<std::string>{"day0", "day1", "day2"}));
    }

    TEST(RetentionPolicyTest, AllFilesAtSameTimestamp) {
      // All files share the same timestamp. The first in the list is kept
      // (newest by position tie-break); all others pruned.
      const std::vector<BackupFile> files{
          at(kDay, "a"),
          at(kDay, "b"),
          at(kDay, "c"),
      };
      EXPECT_EQ(deleted_tags(files, RetentionPolicy{1, 0, 0}, kNever),
                (std::vector<std::string>{"b", "c"}));
    }

  } // namespace
} // namespace fmgr::backup
