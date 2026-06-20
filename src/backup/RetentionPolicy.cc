// SPDX-License-Identifier: AGPL-3.0-or-later

#include "backup/RetentionPolicy.h"

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <unordered_set>
#include <vector>

namespace fmgr::backup {

  namespace {

    constexpr std::int64_t k_micros_per_second = 1'000'000;
    constexpr std::int64_t k_seconds_per_day = 86'400;

    struct Keys {
      std::int64_t day;
      std::int64_t month;
      std::int64_t year;
    };

    Keys bucket_keys(std::int64_t created_micros) {
      const auto seconds = static_cast<std::time_t>(created_micros / k_micros_per_second);
      std::tm tm_utc{};
      ::gmtime_r(&seconds, &tm_utc);
      const std::int64_t year = tm_utc.tm_year + 1900;
      return Keys{
          // floor-divide into whole UTC days so the day bucket is calendar-aligned
          // even for negative epochs (created_micros is non-negative in practice).
          .day = (created_micros / k_micros_per_second) / k_seconds_per_day,
          .month = year * 12 + tm_utc.tm_mon,
          .year = year,
      };
    }

    // For a single tier: keep the newest file in each bucket, restricted to the
    // `limit` most-recent buckets. Adds the kept file indices to `kept`.
    void keep_tier(const std::vector<std::size_t>& order, const std::vector<Keys>& keys,
                   std::int64_t Keys::*field, int limit, std::unordered_set<std::size_t>& kept) {
      if (limit <= 0) {
        return;
      }
      std::unordered_set<std::int64_t> seen_buckets;
      // `order` is newest-first, so the first index encountered for a bucket is
      // that bucket's newest file, and buckets are themselves visited newest-first.
      for (const std::size_t idx : order) {
        const std::int64_t bucket = keys[idx].*field;
        if (seen_buckets.contains(bucket)) {
          continue; // an older file in a bucket already represented
        }
        if (static_cast<int>(seen_buckets.size()) >= limit) {
          break; // beyond the most-recent `limit` buckets for this tier
        }
        seen_buckets.insert(bucket);
        kept.insert(idx);
      }
    }

  } // namespace

  std::vector<BackupFile> select_for_deletion(const std::vector<BackupFile>& files,
                                              const RetentionPolicy& policy,
                                              std::int64_t now_micros) {
    const std::size_t count = files.size();
    std::vector<Keys> keys(count);
    for (std::size_t i = 0; i < count; ++i) {
      keys[i] = bucket_keys(files[i].created_micros);
    }

    // Indices sorted newest-first; tie-break on original position for determinism.
    std::vector<std::size_t> order(count);
    for (std::size_t i = 0; i < count; ++i) {
      order[i] = i;
    }
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
      if (files[a].created_micros != files[b].created_micros) {
        return files[a].created_micros > files[b].created_micros;
      }
      return a < b;
    });

    std::unordered_set<std::size_t> kept;

    // Data-safety floor: never prune the single newest backup, nor any backup
    // dated in the future relative to `now_micros`.
    if (!order.empty()) {
      kept.insert(order.front());
    }
    for (std::size_t i = 0; i < count; ++i) {
      if (files[i].created_micros > now_micros) {
        kept.insert(i);
      }
    }

    keep_tier(order, keys, &Keys::day, policy.daily, kept);
    keep_tier(order, keys, &Keys::month, policy.monthly, kept);
    keep_tier(order, keys, &Keys::year, policy.yearly, kept);

    std::vector<BackupFile> to_delete;
    for (std::size_t i = 0; i < count; ++i) {
      if (!kept.contains(i)) {
        to_delete.push_back(files[i]);
      }
    }
    return to_delete;
  }

} // namespace fmgr::backup
