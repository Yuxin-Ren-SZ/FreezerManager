// SPDX-License-Identifier: AGPL-3.0-or-later

#include "rpc/RateLimiter.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace fmgr::rpc {
  namespace {

    using namespace std::chrono_literals;

    RateLimiter::TimePoint base() {
      return RateLimiter::TimePoint{};
    }

    TEST(RateLimiter, AllowsBurstUpToCapacityThenRejects) {
      RateLimiter limiter(RateLimiterConfig{.capacity = 5, .refill_per_sec = 1});
      const auto now = base();

      // The first `capacity` requests in the same instant are allowed.
      for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(limiter.try_acquire("ip-a", now)) << "burst request " << i;
      }
      // The next is rejected — bucket is empty and no time has passed.
      EXPECT_FALSE(limiter.try_acquire("ip-a", now));
    }

    TEST(RateLimiter, RefillsOverElapsedTime) {
      RateLimiter limiter(RateLimiterConfig{.capacity = 2, .refill_per_sec = 1});
      const auto start = base();

      EXPECT_TRUE(limiter.try_acquire("ip", start));
      EXPECT_TRUE(limiter.try_acquire("ip", start));
      EXPECT_FALSE(limiter.try_acquire("ip", start)); // drained

      // After 1 second, exactly one token has been replenished.
      const auto later = start + 1s;
      EXPECT_TRUE(limiter.try_acquire("ip", later));
      EXPECT_FALSE(limiter.try_acquire("ip", later));
    }

    TEST(RateLimiter, RefillIsCappedAtCapacity) {
      RateLimiter limiter(RateLimiterConfig{.capacity = 3, .refill_per_sec = 10});
      const auto start = base();
      EXPECT_TRUE(limiter.try_acquire("ip", start));

      // A long idle period cannot accumulate more than `capacity` tokens.
      const auto later = start + 100s;
      EXPECT_TRUE(limiter.try_acquire("ip", later));
      EXPECT_TRUE(limiter.try_acquire("ip", later));
      EXPECT_TRUE(limiter.try_acquire("ip", later));
      EXPECT_FALSE(limiter.try_acquire("ip", later));
    }

    TEST(RateLimiter, KeysAreIsolated) {
      RateLimiter limiter(RateLimiterConfig{.capacity = 1, .refill_per_sec = 1});
      const auto now = base();

      EXPECT_TRUE(limiter.try_acquire("ip-a", now));
      EXPECT_FALSE(limiter.try_acquire("ip-a", now)); // a is drained
      EXPECT_TRUE(limiter.try_acquire("ip-b", now));  // b is independent
    }

    TEST(RateLimiter, BoundsTrackedKeysByEvictingLeastRecentlySeen) {
      RateLimiter limiter(
          RateLimiterConfig{.capacity = 1, .refill_per_sec = 1, .max_tracked_keys = 2});
      auto clock = base();

      EXPECT_TRUE(limiter.try_acquire("a", clock));
      clock += 1s;
      EXPECT_TRUE(limiter.try_acquire("b", clock));
      clock += 1s;
      // Inserting a third key evicts the least-recently-seen ("a").
      EXPECT_TRUE(limiter.try_acquire("c", clock));
      EXPECT_EQ(limiter.tracked_keys(), 2U);

      // "a" was evicted, so it gets a fresh full bucket and is allowed again.
      EXPECT_TRUE(limiter.try_acquire("a", clock));
      EXPECT_EQ(limiter.tracked_keys(), 2U);
    }

    TEST(RateLimiter, ClockGoingBackwardsDoesNotReplenish) {
      RateLimiter limiter(RateLimiterConfig{.capacity = 1, .refill_per_sec = 1000});
      const auto start = base() + 10s;
      EXPECT_TRUE(limiter.try_acquire("ip", start));
      // An earlier timestamp must not be treated as elapsed time.
      EXPECT_FALSE(limiter.try_acquire("ip", start - 5s));
    }

    // ---- Concurrency / thread-safety tests ----

    TEST(RateLimiter, IsSafeUnderConcurrentSingleKey) {
      // Multiple threads hammering the same key must never allow more than
      // the total capacity across all threads in a zero-time window.
      RateLimiter limiter(RateLimiterConfig{.capacity = 100.0, .refill_per_sec = 0.0});
      const auto now = base();
      constexpr int kThreads = 8;
      constexpr int kCallsPerThread = 200;

      std::atomic<int> total_allowed{0};
      std::vector<std::thread> threads;
      threads.reserve(kThreads);

      for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&limiter, now, &total_allowed] {
          int local_allowed = 0;
          for (int i = 0; i < kCallsPerThread; ++i) {
            if (limiter.try_acquire("shared-key", now)) {
              ++local_allowed;
            }
          }
          total_allowed.fetch_add(local_allowed, std::memory_order_relaxed);
        });
      }

      for (auto& thread : threads) {
        thread.join();
      }

      // With zero refill, the total allowed across all threads must never
      // exceed the initial capacity of the bucket.
      EXPECT_LE(total_allowed.load(), 100);
      EXPECT_GT(total_allowed.load(), 0);
    }

    TEST(RateLimiter, IsSafeUnderConcurrentDistinctKeys) {
      // Each thread uses its own key — no inter-thread contention on buckets,
      // but the mutex and tracked-key map must still be thread-safe.
      RateLimiter limiter(
          RateLimiterConfig{.capacity = 1.0, .refill_per_sec = 0.0, .max_tracked_keys = 1000});
      const auto now = base();
      constexpr int kThreads = 8;
      constexpr int kKeysPerThread = 50;

      std::atomic<int> total_allowed{0};
      std::vector<std::thread> threads;
      threads.reserve(kThreads);

      for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&limiter, now, &total_allowed, t] {
          int local_allowed = 0;
          for (int k = 0; k < kKeysPerThread; ++k) {
            const auto key = "thread-" + std::to_string(t) + "-key-" + std::to_string(k);
            if (limiter.try_acquire(key, now)) {
              ++local_allowed;
            }
          }
          total_allowed.fetch_add(local_allowed, std::memory_order_relaxed);
        });
      }

      for (auto& thread : threads) {
        thread.join();
      }

      // Each distinct key gets its own fresh full bucket (capacity=1),
      // so each call should be allowed exactly once.
      EXPECT_EQ(total_allowed.load(), kThreads * kKeysPerThread);
    }

    TEST(RateLimiter, EvictionUnderConcurrencyPreservesMaxKeyBound) {
      // Many threads inserting distinct keys must not cause the tracked-key
      // map to exceed max_tracked_keys, even under concurrent insertion.
      RateLimiter limiter(
          RateLimiterConfig{.capacity = 1.0, .refill_per_sec = 1.0, .max_tracked_keys = 50});
      auto clock = base();
      constexpr int kThreads = 4;
      constexpr int kOpsPerThread = 500;

      std::vector<std::thread> threads;
      threads.reserve(kThreads);

      for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&limiter, &clock, t] {
          for (int i = 0; i < kOpsPerThread; ++i) {
            const auto key =
                "t" + std::to_string(t) + "-k" + std::to_string(i % 200);
            limiter.try_acquire(key, clock);
            // We can't safely advance clock here (shared mutable) but the
            // invariant is that tracked_keys never exceeds max.
          }
        });
      }

      for (auto& thread : threads) {
        thread.join();
      }

      EXPECT_LE(limiter.tracked_keys(), 50U);
    }

  } // namespace
} // namespace fmgr::rpc
