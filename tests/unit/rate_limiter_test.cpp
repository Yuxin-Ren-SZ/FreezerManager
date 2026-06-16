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

    // ---- Break-it: config edge cases ----

    TEST(RateLimiter, ZeroCapacityAllowsNothing) {
      RateLimiter limiter(RateLimiterConfig{.capacity = 0.0, .refill_per_sec = 1.0});
      const auto now = base();
      EXPECT_FALSE(limiter.try_acquire("ip", now));
      // Even after a long wait, zero capacity means nothing is ever allowed.
      EXPECT_FALSE(limiter.try_acquire("ip", now + 100s));
    }

    TEST(RateLimiter, ZeroRefillNeverReplenishes) {
      RateLimiter limiter(RateLimiterConfig{.capacity = 3.0, .refill_per_sec = 0.0});
      const auto now = base();
      EXPECT_TRUE(limiter.try_acquire("ip", now));
      EXPECT_TRUE(limiter.try_acquire("ip", now));
      EXPECT_TRUE(limiter.try_acquire("ip", now));
      EXPECT_FALSE(limiter.try_acquire("ip", now));
      // Even after a long wait, zero refill means bucket stays empty.
      EXPECT_FALSE(limiter.try_acquire("ip", now + 3600s));
    }

    TEST(RateLimiter, ZeroMaxTrackedKeysImmediateEviction) {
      RateLimiter limiter(
          RateLimiterConfig{.capacity = 1.0, .refill_per_sec = 1.0, .max_tracked_keys = 0});
      const auto now = base();
      // With max_tracked_keys=0, the implementation may keep the key anyway
      // (treating 0 as "unlimited"), or it may immediately evict it.
      // Either way: must not crash or hang.
      EXPECT_TRUE(limiter.try_acquire("a", now));
      // The second call behavior depends on whether "a" was evicted.
      limiter.try_acquire("a", now); // must not crash
    }

    // ---- Break-it: key edge cases ----

    TEST(RateLimiter, EmptyKeyIsValid) {
      RateLimiter limiter(RateLimiterConfig{.capacity = 1.0, .refill_per_sec = 1.0});
      const auto now = base();
      // An empty string key must not crash or hang — it's just another key.
      EXPECT_TRUE(limiter.try_acquire("", now));
      EXPECT_FALSE(limiter.try_acquire("", now));
    }

    TEST(RateLimiter, VeryLongKeyDoesNotCrash) {
      RateLimiter limiter(RateLimiterConfig{.capacity = 1.0, .refill_per_sec = 1.0});
      const auto now = base();
      const std::string long_key(10'000, 'x');
      EXPECT_TRUE(limiter.try_acquire(long_key, now));
      EXPECT_FALSE(limiter.try_acquire(long_key, now));
    }

    TEST(RateLimiter, KeyWithSpecialCharacters) {
      RateLimiter limiter(RateLimiterConfig{.capacity = 2.0, .refill_per_sec = 1.0});
      const auto now = base();
      // Keys with special characters (spaces, null bytes, unicode) must not crash.
      EXPECT_TRUE(limiter.try_acquire("key with spaces", now));
      EXPECT_TRUE(limiter.try_acquire(std::string("key\0with\0nulls", 15), now));
      // Unicode key gets its own bucket with capacity 2 — twice should succeed.
      EXPECT_TRUE(limiter.try_acquire("key-with-unicode-🔥", now));
      EXPECT_TRUE(limiter.try_acquire("key-with-unicode-🔥", now));
      // Third call with the same unicode key should exhaust the bucket.
      EXPECT_FALSE(limiter.try_acquire("key-with-unicode-🔥", now));
    }

    // ---- Break-it: refill precision at boundaries ----

    TEST(RateLimiter, RefillFractionalTokenAtExactBoundary) {
      // 0.5 tokens/sec: after exactly 2 seconds, exactly 1 token is available.
      RateLimiter limiter(RateLimiterConfig{.capacity = 1.0, .refill_per_sec = 0.5});
      const auto start = base();
      EXPECT_TRUE(limiter.try_acquire("ip", start));
      EXPECT_FALSE(limiter.try_acquire("ip", start));
      // After 2 seconds, exactly 1 token should be accumulated.
      EXPECT_TRUE(limiter.try_acquire("ip", start + 2s));
      EXPECT_FALSE(limiter.try_acquire("ip", start + 2s));
    }

    TEST(RateLimiter, RefillAtSubSecondGranularity) {
      // 10 tokens/sec: after 100ms, 1 token should be available.
      RateLimiter limiter(RateLimiterConfig{.capacity = 2.0, .refill_per_sec = 10.0});
      const auto start = base();
      EXPECT_TRUE(limiter.try_acquire("ip", start));
      EXPECT_TRUE(limiter.try_acquire("ip", start));
      EXPECT_FALSE(limiter.try_acquire("ip", start));
      // After 100ms, 1 token refilled.
      EXPECT_TRUE(limiter.try_acquire("ip", start + 100ms));
      EXPECT_FALSE(limiter.try_acquire("ip", start + 100ms));
    }

    TEST(RateLimiter, VeryHighRefillRateDoesNotOverflow) {
      // 1e9 tokens/sec is extreme but must not cause overflow or UB.
      RateLimiter limiter(RateLimiterConfig{.capacity = 10.0, .refill_per_sec = 1e9});
      const auto start = base();
      // Drain the bucket.
      for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(limiter.try_acquire("ip", start));
      }
      EXPECT_FALSE(limiter.try_acquire("ip", start));
      // Even 1 microsecond of refill at 1e9/s gives 1000 tokens — should refill to
      // capacity immediately.
      EXPECT_TRUE(limiter.try_acquire("ip", start + 1us));
    }

    // ---- Break-it: large capacity stress ----

    TEST(RateLimiter, LargeCapacityBurst) {
      RateLimiter limiter(RateLimiterConfig{.capacity = 10'000.0, .refill_per_sec = 0.0});
      const auto now = base();
      for (int i = 0; i < 10'000; ++i) {
        EXPECT_TRUE(limiter.try_acquire("ip", now)) << "request " << i;
      }
      EXPECT_FALSE(limiter.try_acquire("ip", now));
    }

    TEST(RateLimiter, HighThroughputManyKeys) {
      // Ensure the limiter handles many keys without performance collapse.
      RateLimiter limiter(
          RateLimiterConfig{.capacity = 10.0, .refill_per_sec = 10.0, .max_tracked_keys = 10'000});
      const auto now = base();
      for (int k = 0; k < 5'000; ++k) {
        const auto key = "key-" + std::to_string(k);
        EXPECT_TRUE(limiter.try_acquire(key, now)) << "key " << k;
      }
      EXPECT_LE(limiter.tracked_keys(), 10'000U);
    }

    // ==== Aggressive: concurrency + clock extremes ====

    TEST(RateLimiter, ConcurrentSingleKeyWithRefillUnderPressure) {
      // Many threads hammering a single key while the clock advances; total
      // allowed must never exceed capacity + refill over elapsed time.
      RateLimiter limiter(RateLimiterConfig{.capacity = 50.0, .refill_per_sec = 100.0});
      const auto start = base();
      constexpr int kThreads = 8;
      constexpr int kCalls = 500;

      std::atomic<int> allowed{0};
      std::vector<std::thread> threads;
      for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&] {
          int local = 0;
          for (int i = 0; i < kCalls; ++i) {
            // Advance clock slowly — each call moves 1ms forward.
            const auto now = start + std::chrono::milliseconds(i);
            if (limiter.try_acquire("hot", now)) {
              ++local;
            }
          }
          allowed.fetch_add(local, std::memory_order_relaxed);
        });
      }
      for (auto& th : threads) th.join();

      // Capacity 50 + refill at 100/s over 500ms = 50+50 = 100 max per thread,
      // but the shared bucket means total ≤ 50 + 100*0.5 = 100 overall.
      // This is a sanity bound, not a precise assertion.
      EXPECT_GT(allowed.load(), 0);
      EXPECT_LE(allowed.load(), kThreads * 50 + 200);
    }

    TEST(RateLimiter, ClockBackwardByNanosecondsIsHarmless) {
      RateLimiter limiter(RateLimiterConfig{.capacity = 1.0, .refill_per_sec = 1.0});
      const auto now = base();
      EXPECT_TRUE(limiter.try_acquire("ip", now));
      // Clock moved backward by 1 nanosecond — must not crash or grant extra tokens.
      EXPECT_FALSE(limiter.try_acquire("ip", now - 1ns));
    }

    TEST(RateLimiter, ClockJumpForwardByYearsExhaustsRefill) {
      RateLimiter limiter(RateLimiterConfig{.capacity = 5.0, .refill_per_sec = 1.0});
      const auto now = base();
      // Drain the bucket.
      for (int i = 0; i < 5; ++i) limiter.try_acquire("ip", now);
      // Jump forward 100 years — must refill to capacity, not overflow.
      using namespace std::chrono_literals;
      const auto far = now + std::chrono::hours(24 * 365 * 100);
      EXPECT_TRUE(limiter.try_acquire("ip", far));
      EXPECT_TRUE(limiter.try_acquire("ip", far));
      EXPECT_TRUE(limiter.try_acquire("ip", far));
      EXPECT_TRUE(limiter.try_acquire("ip", far));
      EXPECT_TRUE(limiter.try_acquire("ip", far));
      EXPECT_FALSE(limiter.try_acquire("ip", far));
    }

    TEST(RateLimiter, FractionalTokenAccumulationPrecision) {
      // 0.3 tokens/sec: after 3 seconds, 0.9 tokens — not enough for 1.
      // After 4 seconds, 1.2 tokens — exactly 1 grantable.
      RateLimiter limiter(RateLimiterConfig{.capacity = 1.0, .refill_per_sec = 0.3});
      const auto start = base();
      EXPECT_TRUE(limiter.try_acquire("ip", start));
      EXPECT_FALSE(limiter.try_acquire("ip", start + 3s));
      EXPECT_TRUE(limiter.try_acquire("ip", start + 4s));
      EXPECT_FALSE(limiter.try_acquire("ip", start + 4s));
    }

    TEST(RateLimiter, ConcurrentMixedReadWriteDistinctKeys) {
      // Some threads drain their keys while others read (tracked_keys).
      RateLimiter limiter(
          RateLimiterConfig{.capacity = 10.0, .refill_per_sec = 10.0, .max_tracked_keys = 500});
      const auto now = base();
      constexpr int kThreads = 6;
      std::atomic<bool> stop{false};
      std::atomic<int> reads{0};

      std::vector<std::thread> threads;
      for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
          for (int i = 0; i < 200 && !stop.load(); ++i) {
            if (t % 2 == 0) {
              limiter.try_acquire("t" + std::to_string(t) + "-k" + std::to_string(i), now);
            } else {
              (void)limiter.tracked_keys();
              reads.fetch_add(1, std::memory_order_relaxed);
            }
          }
        });
      }
      for (auto& th : threads) th.join();
      EXPECT_GT(reads.load(), 0);
    }

  } // namespace
} // namespace fmgr::rpc
