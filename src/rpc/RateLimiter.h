// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FMGR_RPC_RATELIMITER_H
#define FMGR_RPC_RATELIMITER_H

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>

namespace fmgr::rpc {

  // Token-bucket rate limiter keyed by an opaque string (e.g. a client IP).
  //
  // Defends the auth surface against credential-spray / account-enumeration:
  // the per-email lockout caps failures for one account, but does nothing to
  // throttle the *volume* of attempts an attacker can spread across thousands
  // of distinct emails (security audit H-1). This limiter throttles by source.
  //
  // The clock is injected (every call takes a `now`) so behaviour is fully
  // deterministic and unit-testable; production callers pass
  // std::chrono::steady_clock::now(). The tracked-key map is bounded: once it
  // reaches `max_tracked_keys`, the least-recently-seen key is evicted before a
  // new one is inserted, so a spray across unbounded distinct IPs cannot grow
  // memory without limit (mirrors the lockout-map cap).
  struct RateLimiterConfig {
    // Maximum burst: the bucket holds at most this many tokens.
    double capacity{30.0};
    // Sustained allowed rate: tokens replenished per second.
    double refill_per_sec{5.0};
    // Upper bound on distinct keys tracked at once.
    std::size_t max_tracked_keys{10000};
  };

  class RateLimiter {
  public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    explicit RateLimiter(RateLimiterConfig config) : config_(config) {}

    // Attempt to consume one token for `key` at time `now`. Returns true if a
    // token was available (request allowed), false if the bucket is empty
    // (request should be rejected with RESOURCE_EXHAUSTED). Thread-safe.
    [[nodiscard]] bool try_acquire(const std::string& key, TimePoint now) {
      const std::scoped_lock lock(mutex_);

      auto it = buckets_.find(key);
      if (it == buckets_.end()) {
        if (buckets_.size() >= config_.max_tracked_keys) {
          evict_least_recently_seen();
        }
        // A fresh bucket starts full, then immediately spends one token.
        it = buckets_.emplace(key, Bucket{.tokens = config_.capacity, .last_refill = now}).first;
      } else {
        refill(it->second, now);
      }

      it->second.last_seen = now;
      if (it->second.tokens >= 1.0) {
        it->second.tokens -= 1.0;
        return true;
      }
      return false;
    }

    [[nodiscard]] std::size_t tracked_keys() const {
      const std::scoped_lock lock(mutex_);
      return buckets_.size();
    }

  private:
    struct Bucket {
      double tokens{0.0};
      TimePoint last_refill{};
      TimePoint last_seen{};
    };

    void refill(Bucket& bucket, TimePoint now) const {
      if (now <= bucket.last_refill) {
        return; // clock did not advance (or went backwards): no replenishment
      }
      const std::chrono::duration<double> elapsed = now - bucket.last_refill;
      bucket.tokens =
          std::min(config_.capacity, bucket.tokens + elapsed.count() * config_.refill_per_sec);
      bucket.last_refill = now;
    }

    void evict_least_recently_seen() {
      auto oldest = buckets_.begin();
      for (auto it = buckets_.begin(); it != buckets_.end(); ++it) {
        if (it->second.last_seen < oldest->second.last_seen) {
          oldest = it;
        }
      }
      if (oldest != buckets_.end()) {
        buckets_.erase(oldest);
      }
    }

    RateLimiterConfig config_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Bucket> buckets_;
  };

} // namespace fmgr::rpc

#endif // FMGR_RPC_RATELIMITER_H
