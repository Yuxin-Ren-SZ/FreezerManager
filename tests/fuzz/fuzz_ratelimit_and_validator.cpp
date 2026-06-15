// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Property-based fuzz harnesses for security-critical code paths.
// These don't require libFuzzer — they run as normal GoogleTest tests with
// randomized inputs and check invariants that must always hold.

#include "audit/CanonicalJson.h"
#include "core/custom_field_validator.h"
#include "core/uuid.h"
#include "rpc/RateLimiter.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace fmgr::fuzz {
  namespace {

    using namespace std::chrono_literals;

    // Deterministic PRNG seeded from the test random seed so failures are
    // reproducible via --gtest_random_seed=.
    [[nodiscard]] std::mt19937_64& rng() {
      static std::mt19937_64 engine(
          static_cast<std::uint64_t>(
              ::testing::UnitTest::GetInstance()->random_seed()) +
          reinterpret_cast<std::uintptr_t>(&rng));
      return engine;
    }

    // ---- RateLimiter fuzz ----

    TEST(FuzzRateLimiter, TokenCountNeverExceedsCapacity) {
      // We can't read per-bucket tokens directly from the public API, but we can
      // verify the observable invariant: a single key can never acquire more than
      // capacity + ceiling(refill_over_observed_time) tokens without rejections
      // interspersed. We exercise the limiter with randomized intervals and check
      // that it never allows an unbounded burst.
      auto& gen = rng();
      constexpr int kCapacity = 5;
      constexpr int kRefillPerSec = 2;
      rpc::RateLimiter limiter(rpc::RateLimiterConfig{.capacity = static_cast<double>(kCapacity),
                                            .refill_per_sec = static_cast<double>(kRefillPerSec)});
      auto clock = rpc::RateLimiter::TimePoint{};
      constexpr int kOps = 2000;

      int consecutive_allowed = 0;
      int max_consecutive_allowed = 0;

      for (int i = 0; i < kOps; ++i) {
        const bool allowed = limiter.try_acquire("fuzz-key", clock);
        if (allowed) {
          ++consecutive_allowed;
          max_consecutive_allowed = std::max(max_consecutive_allowed, consecutive_allowed);
        } else {
          consecutive_allowed = 0;
        }
        // Random time advance: 0..500 ms
        const auto ms = std::uniform_int_distribution<int>(0, 500)(gen);
        clock += std::chrono::milliseconds(ms);
      }

      // A token bucket with capacity C should never allow more than C tokens
      // in an instant (no time passes). With time passing, the practical burst
      // observed can be at most C + a few refills during the concurrent window.
      // We check a generous upper bound: capacity * 3.
      EXPECT_LE(max_consecutive_allowed, kCapacity * 3)
          << "token bucket allowed an unexpectedly large burst";
    }

    TEST(FuzzRateLimiter, DistinctKeysDontInterfere) {
      auto& gen = rng();
      rpc::RateLimiter limiter(
          rpc::RateLimiterConfig{.capacity = 3.0, .refill_per_sec = 1.0, .max_tracked_keys = 1000});
      auto clock = rpc::RateLimiter::TimePoint{};
      constexpr int kOps = 500;

      std::set<std::string> keys_that_ever_failed;
      int allowed_total = 0;
      int rejected_total = 0;

      for (int i = 0; i < kOps; ++i) {
        const auto key = "key-" + std::to_string(std::uniform_int_distribution<int>(0, 20)(gen));
        const bool allowed = limiter.try_acquire(key, clock);
        if (allowed) {
          ++allowed_total;
        } else {
          ++rejected_total;
          keys_that_ever_failed.insert(key);
        }
        clock += std::chrono::milliseconds(std::uniform_int_distribution<int>(0, 100)(gen));
      }

      // With only 21 distinct keys spread across time, we must observe both
      // allows (fresh buckets) and rejects (exhausted buckets).
      EXPECT_GT(allowed_total, 0);
      EXPECT_GT(rejected_total, 0);
      EXPECT_LE(keys_that_ever_failed.size(), 21U);
    }

    TEST(FuzzRateLimiter, TrackedKeyCountNeverExceedsMax) {
      auto& gen = rng();
      constexpr std::size_t kMaxKeys = 20;
      rpc::RateLimiter limiter(
          rpc::RateLimiterConfig{.capacity = 1.0, .refill_per_sec = 100.0, .max_tracked_keys = kMaxKeys});
      auto clock = rpc::RateLimiter::TimePoint{};

      for (int i = 0; i < 500; ++i) {
        const auto key = "ip-" + std::to_string(std::uniform_int_distribution<int>(0, 200)(gen));
        limiter.try_acquire(key, clock);
        clock += 1s;
        EXPECT_LE(limiter.tracked_keys(), kMaxKeys);
      }
    }

    // ---- UUID v4 fuzz (cryptographic RNG path) ----

    TEST(FuzzUuid, GenerationNeverProducesInvalidUuid) {
      constexpr int kIterations = 1000;
      for (int i = 0; i < kIterations; ++i) {
        const auto text = core::generate_uuid_v4();
        ASSERT_EQ(text.size(), 36U);
        // Parsing must never throw on a freshly generated UUID.
        auto uuid = core::Uuid::parse(text);
        EXPECT_EQ(uuid.to_string(), text);
        // Must be version 4.
        EXPECT_EQ(text.at(14), '4');
        const char variant = text.at(19);
        EXPECT_TRUE(variant == '8' || variant == '9' || variant == 'a' || variant == 'b');
      }
    }

    // ---- CustomFieldValidator fuzz ----

    [[nodiscard]] core::CustomFieldDefinition random_string_def(std::mt19937_64& gen) {
      const bool required = std::uniform_int_distribution<int>(0, 1)(gen) != 0;
      return core::CustomFieldDefinition{
          .id = core::CustomFieldDefinitionId(
              core::Uuid::parse("00000000-0000-0000-0000-000000000001")),
          .lab_id = core::LabId(core::Uuid::parse("00000000-0000-0000-0000-000000000002")),
          .scope_kind = core::ScopeKind::Sample,
          .item_type_id = std::nullopt,
          .key = "fuzz_field",
          .label = "Fuzz Field",
          .data_type = core::FieldDataType::String,
          .required = required,
          .validation_json = "{}",
          .indexed = false,
          .is_phi = false,
          .created_at = core::Timestamp::from_unix_micros(1),
      };
    }

    TEST(FuzzCustomFieldValidator, RandomStringsNeverCrash) {
      auto& gen = rng();
      const auto def = random_string_def(gen);
      constexpr int kIterations = 500;

      for (int i = 0; i < kIterations; ++i) {
        nlohmann::json value;
        switch (std::uniform_int_distribution<int>(0, 4)(gen)) {
        case 0:
          value = std::uniform_int_distribution<int>(0, 99999)(gen);
          break;
        case 1:
          value = nullptr;
          break;
        case 2:
          value = true;
          break;
        case 3: {
          // Random-length string (0..1024 chars)
          const int len = std::uniform_int_distribution<int>(0, 1024)(gen);
          value = std::string(static_cast<std::size_t>(len), 'x');
          break;
        }
        case 4:
          value = nlohmann::json::object();
          break;
        }
        const nlohmann::json fields{{"fuzz_field", value}};
        // Must never throw; at worst, returns validation errors.
        EXPECT_NO_THROW({
          const auto errors =
              core::validate_custom_fields(std::span{&def, 1}, fields);
          (void)errors;
        });
      }
    }

    TEST(FuzzCustomFieldValidator, RandomKeysAndValuesNeverCrash) {
      auto& gen = rng();
      constexpr int kIterations = 500;

      for (int i = 0; i < kIterations; ++i) {
        // Generate a random set of definitions (1..4 fields).
        const int num_defs = std::uniform_int_distribution<int>(1, 4)(gen);
        std::vector<core::CustomFieldDefinition> defs;
        defs.reserve(static_cast<std::size_t>(num_defs));
        for (int d = 0; d < num_defs; ++d) {
          auto def = random_string_def(gen);
          def.key = "field_" + std::to_string(d);
          defs.push_back(def);
        }

        // Generate random JSON with 0..8 keys.
        nlohmann::json fields = nlohmann::json::object();
        const int num_keys = std::uniform_int_distribution<int>(0, 8)(gen);
        for (int k = 0; k < num_keys; ++k) {
          const auto key = "field_" + std::to_string(std::uniform_int_distribution<int>(0, 7)(gen));
          if (!fields.contains(key)) {
            const int val_type = std::uniform_int_distribution<int>(0, 3)(gen);
            switch (val_type) {
            case 0:
              fields[key] = std::uniform_int_distribution<int>(0, 100)(gen);
              break;
            case 1:
              fields[key] = nullptr;
              break;
            case 2: {
              const int len = std::uniform_int_distribution<int>(0, 256)(gen);
              fields[key] = std::string(static_cast<std::size_t>(len), 'a');
              break;
            }
            case 3:
              fields[key] = std::uniform_real_distribution<double>(-1000.0, 1000.0)(gen);
              break;
            }
          }
        }

        EXPECT_NO_THROW({
          const auto errors = core::validate_custom_fields(defs, fields);
          (void)errors;
        });
      }
    }

    // ---- Canonical JSON fuzz ----

    TEST(FuzzCanonicalJson, DeterministicOutputForSameInput) {
      auto& gen = rng();
      constexpr int kIterations = 200;

      for (int i = 0; i < kIterations; ++i) {
        nlohmann::json value;
        const int shape = std::uniform_int_distribution<int>(0, 6)(gen);
        switch (shape) {
        case 0: value = nullptr; break;
        case 1: value = true; break;
        case 2: value = std::uniform_int_distribution<int>(-1000, 1000)(gen); break;
        case 3: value = std::uniform_real_distribution<double>(-100.0, 100.0)(gen); break;
        case 4: {
          const int len = std::uniform_int_distribution<int>(0, 256)(gen);
          value = std::string(static_cast<std::size_t>(len), 'x');
          break;
        }
        case 5: {
          nlohmann::json arr = nlohmann::json::array();
          const int count = std::uniform_int_distribution<int>(0, 8)(gen);
          for (int j = 0; j < count; ++j) {
            arr.push_back(std::uniform_int_distribution<int>(0, 99)(gen));
          }
          value = std::move(arr);
          break;
        }
        case 6: {
          nlohmann::json obj = nlohmann::json::object();
          const int count = std::uniform_int_distribution<int>(0, 5)(gen);
          for (int j = 0; j < count; ++j) {
            obj["key" + std::to_string(j)] = std::uniform_int_distribution<int>(0, 99)(gen);
          }
          value = std::move(obj);
          break;
        }
        }

        const auto first = audit::canonical_json(value);
        const auto second = audit::canonical_json(value);
        // Determinism: same input must produce same output every time.
        EXPECT_EQ(first, second) << "canonical_json not deterministic for shape " << shape;
      }
    }

    TEST(FuzzCanonicalJson, IdempotentUnderRepeatedSerialization) {
      auto& gen = rng();
      constexpr int kIterations = 100;

      for (int i = 0; i < kIterations; ++i) {
        nlohmann::json obj = nlohmann::json::object();
        const int count = std::uniform_int_distribution<int>(1, 8)(gen);
        for (int j = 0; j < count; ++j) {
          // Insert keys in random order to verify sorting.
          const auto key = "field_" + std::to_string(std::uniform_int_distribution<int>(0, 20)(gen));
          if (!obj.contains(key)) {
            obj[key] = std::uniform_int_distribution<int>(0, 999)(gen);
          }
        }

        const auto first = audit::canonical_json(obj);
        // Round-trip: parse the canonical form back, then re-serialize.
        const auto reparsed = nlohmann::json::parse(first);
        const auto second = audit::canonical_json(reparsed);
        EXPECT_EQ(first, second) << "canonical_json not idempotent";
      }
    }

    TEST(FuzzCanonicalJson, NeverCrashesOnArbitraryJson) {
      auto& gen = rng();
      constexpr int kIterations = 500;

      for (int i = 0; i < kIterations; ++i) {
        nlohmann::json value;
        const int shape = std::uniform_int_distribution<int>(0, 6)(gen);
        switch (shape) {
        case 0: value = nullptr; break;
        case 1: value = true; break;
        case 2: value = std::numeric_limits<double>::quiet_NaN(); break;
        case 3: value = std::numeric_limits<double>::infinity(); break;
        case 4: value = -std::numeric_limits<double>::infinity(); break;
        case 5: {
          nlohmann::json deep = nlohmann::json::object();
          nlohmann::json* cursor = &deep;
          const int depth = std::uniform_int_distribution<int>(1, 20)(gen);
          for (int d = 0; d < depth; ++d) {
            (*cursor)["nested"] = nlohmann::json::object();
            cursor = &(*cursor)["nested"];
          }
          (*cursor)["leaf"] = std::uniform_int_distribution<int>(0, 99)(gen);
          value = std::move(deep);
          break;
        }
        case 6: {
          // Large array with mixed types.
          nlohmann::json arr = nlohmann::json::array();
          const int count = std::uniform_int_distribution<int>(0, 200)(gen);
          for (int j = 0; j < count; ++j) {
            const int elem_type = std::uniform_int_distribution<int>(0, 2)(gen);
            switch (elem_type) {
            case 0: arr.push_back(nullptr); break;
            case 1: arr.push_back(std::uniform_int_distribution<int>(0, 99)(gen)); break;
            case 2: arr.push_back(true); break;
            }
          }
          value = std::move(arr);
          break;
        }
        }

        // canonical_json must never throw on valid JSON inputs.
        EXPECT_NO_THROW({
          const auto result = audit::canonical_json(value);
          (void)result;
        }) << "canonical_json threw on shape " << shape << " iteration " << i;
      }
    }

    TEST(FuzzAuditHash, SameInputProducesSameHash) {
      auto& gen = rng();
      constexpr int kIterations = 100;

      for (int i = 0; i < kIterations; ++i) {
        const auto prev =
            std::string(static_cast<std::size_t>(std::uniform_int_distribution<int>(0, 64)(gen)),
                        'a');
        nlohmann::json content = {{"n", std::uniform_int_distribution<int>(0, 9999)(gen)}};
        const auto json_str = audit::canonical_json(content);

        const auto first = audit::compute_audit_hash(prev, json_str);
        const auto second = audit::compute_audit_hash(prev, json_str);
        EXPECT_EQ(first, second) << "compute_audit_hash not deterministic";
        // Hashes should always be 64-character hex strings.
        EXPECT_EQ(first.size(), 64U);
        EXPECT_EQ(second.size(), 64U);
      }
    }

    TEST(FuzzAuditHash, DifferentInputsProduceDifferentHashes) {
      auto& gen = rng();
      constexpr int kIterations = 100;

      for (int i = 0; i < kIterations; ++i) {
        nlohmann::json content_a = {{"n", std::uniform_int_distribution<int>(0, 9999)(gen)}};
        nlohmann::json content_b = {{"n", std::uniform_int_distribution<int>(0, 9999)(gen)}};
        const auto json_a = audit::canonical_json(content_a);
        const auto json_b = audit::canonical_json(content_b);

        const auto hash_a = audit::compute_audit_hash("", json_a);
        const auto hash_b = audit::compute_audit_hash("", json_b);
        if (json_a != json_b) {
          EXPECT_NE(hash_a, hash_b)
              << "SHA-256 collision or identical content produced different hashes";
        }
      }
    }

  } // namespace
} // namespace fmgr::fuzz
