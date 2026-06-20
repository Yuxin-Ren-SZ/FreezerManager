// SPDX-License-Identifier: AGPL-3.0-or-later

#include "backup/BackupFilename.h"

#include <cstdint>
#include <string>

#include <gtest/gtest.h>

namespace fmgr::backup {
  namespace {

    constexpr std::int64_t kMicros = 1'000'000;

    // 2026-06-19T03:15:00Z == 1781838900 seconds since the epoch.
    constexpr std::int64_t kKnownSeconds = 1781838900;

    TEST(BackupFilenameTest, FormatsKnownTimestamp) {
      EXPECT_EQ(make_backup_filename(kKnownSeconds * kMicros), "fmgr-20260619T031500Z.fmgrbak");
    }

    TEST(BackupFilenameTest, TruncatesSubSecond) {
      EXPECT_EQ(make_backup_filename(kKnownSeconds * kMicros + 999'999),
                "fmgr-20260619T031500Z.fmgrbak");
    }

    TEST(BackupFilenameTest, RoundTripsAtSecondGranularity) {
      const std::int64_t micros = kKnownSeconds * kMicros + 123'456;
      const std::string name = make_backup_filename(micros);
      const auto parsed = parse_backup_timestamp(name);
      ASSERT_TRUE(parsed.has_value());
      EXPECT_EQ(*parsed, kKnownSeconds * kMicros); // sub-second dropped
    }

    TEST(BackupFilenameTest, RoundTripsEpoch) {
      const std::string name = make_backup_filename(0);
      EXPECT_EQ(name, "fmgr-19700101T000000Z.fmgrbak");
      const auto parsed = parse_backup_timestamp(name);
      ASSERT_TRUE(parsed.has_value());
      EXPECT_EQ(*parsed, 0);
    }

    TEST(BackupFilenameTest, RejectsWrongPrefixOrSuffix) {
      EXPECT_FALSE(parse_backup_timestamp("dump-20260619T031500Z.fmgrbak").has_value());
      EXPECT_FALSE(parse_backup_timestamp("fmgr-20260619T031500Z.bak").has_value());
      EXPECT_FALSE(parse_backup_timestamp("fmgr-20260619T031500Z.fmgrbak.tmp").has_value());
    }

    TEST(BackupFilenameTest, RejectsMalformedStamp) {
      EXPECT_FALSE(
          parse_backup_timestamp("fmgr-2026-619T031500Z.fmgrbak").has_value()); // wrong sep
      EXPECT_FALSE(parse_backup_timestamp("fmgr-20260619X031500Z.fmgrbak").has_value()); // no 'T'
      EXPECT_FALSE(parse_backup_timestamp("fmgr-20260619T031500A.fmgrbak").has_value()); // no 'Z'
      EXPECT_FALSE(parse_backup_timestamp("fmgr-202606190031500Z.fmgrbak").has_value()); // length
    }

    TEST(BackupFilenameTest, RejectsOutOfRangeFields) {
      EXPECT_FALSE(parse_backup_timestamp("fmgr-20261319T031500Z.fmgrbak").has_value()); // month 13
      EXPECT_FALSE(parse_backup_timestamp("fmgr-20260632T031500Z.fmgrbak").has_value()); // day 32
      EXPECT_FALSE(parse_backup_timestamp("fmgr-20260619T251500Z.fmgrbak").has_value()); // hour 25
    }

    TEST(BackupFilenameTest, RejectsEmpty) {
      EXPECT_FALSE(parse_backup_timestamp("").has_value());
    }

  } // namespace
} // namespace fmgr::backup
