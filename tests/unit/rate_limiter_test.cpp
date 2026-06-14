// SPDX-License-Identifier: AGPL-3.0-or-later

#include "rpc/RateLimiter.h"

#include <gtest/gtest.h>

#include <chrono>
#include <string>

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

  } // namespace
} // namespace fmgr::rpc
