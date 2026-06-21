// SPDX-License-Identifier: AGPL-3.0-or-later

#include "audit/AuditChainVerifier.h"
#include "audit/CanonicalJson.h"
#include "core/audit_event.h"

#include "test_helpers.h"
#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

namespace fmgr::audit {
  namespace {
    using namespace fmgr::test;

    [[nodiscard]] core::AuditEvent make_event(std::uint64_t low, std::string action = "insert") {
      core::AuditEvent event;
      event.id = id_from_low<core::AuditEventId>(low);
      event.at = ts(1'000 + static_cast<std::int64_t>(low));
      event.actor_user_id = id_from_low<core::UserId>(low);
      event.actor_session_id = "session-" + std::to_string(low);
      event.lab_id = id_from_low<core::LabId>(1);
      event.action = std::move(action);
      event.entity_kind = "sample";
      event.entity_id = id_from_low<core::SampleId>(low).to_string();
      event.before_json = "{}";
      event.after_json = R"({"name":"s)" + std::to_string(low) + "\"}";
      event.request_id = "req-" + std::to_string(low);
      return event;
    }

    // Link a run of events into a valid chain, computing prev/this hashes the
    // same way the storage backends do.
    void chain(std::vector<core::AuditEvent>& events, std::string_view first_prev = zero_hash()) {
      std::string prev(first_prev);
      for (auto& event : events) {
        event.prev_hash = prev;
        event.this_hash = compute_audit_hash(prev, audit_event_content_json(event));
        prev = event.this_hash;
      }
    }

    TEST(AuditChainVerifierTest, EmptyChainIsOk) {
      const auto report = verify_audit_chain({});
      EXPECT_TRUE(report.ok);
      EXPECT_EQ(report.verified_count, 0U);
      EXPECT_FALSE(report.first_error.has_value());
    }

    TEST(AuditChainVerifierTest, SingleRowVerifies) {
      std::vector<core::AuditEvent> events = {make_event(1)};
      chain(events);
      const auto report = verify_audit_chain(events);
      EXPECT_TRUE(report.ok);
      EXPECT_EQ(report.verified_count, 1U);
    }

    TEST(AuditChainVerifierTest, ValidChainVerifies) {
      std::vector<core::AuditEvent> events = {make_event(1), make_event(2), make_event(3)};
      chain(events);
      const auto report = verify_audit_chain(events);
      EXPECT_TRUE(report.ok);
      EXPECT_EQ(report.verified_count, 3U);
      EXPECT_FALSE(report.first_error.has_value());
    }

    TEST(AuditChainVerifierTest, TamperedFieldIsHashMismatchAtThatRow) {
      std::vector<core::AuditEvent> events = {make_event(1), make_event(2), make_event(3)};
      chain(events);
      // Mutate a hashed field on row 1 WITHOUT rechaining: this_hash now stale.
      events[1].action = "tampered";
      const auto report = verify_audit_chain(events);
      ASSERT_FALSE(report.ok);
      ASSERT_TRUE(report.first_error.has_value());
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access): guarded by ASSERT_TRUE above
      const auto& err = report.first_error.value();
      EXPECT_EQ(err.index, 1U);
      EXPECT_EQ(err.id, events[1].id);
      EXPECT_EQ(err.status, AuditChainStatus::HashMismatch);
      EXPECT_EQ(report.verified_count, 1U); // row 0 verified before the failure
    }

    TEST(AuditChainVerifierTest, TamperedAfterJsonIsDetected) {
      std::vector<core::AuditEvent> events = {make_event(1), make_event(2)};
      chain(events);
      events[0].after_json = R"({"name":"evil"})";
      const auto report = verify_audit_chain(events);
      ASSERT_FALSE(report.ok);
      ASSERT_TRUE(report.first_error.has_value());
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access): guarded by ASSERT_TRUE above
      const auto& err = report.first_error.value();
      EXPECT_EQ(err.index, 0U);
      EXPECT_EQ(err.status, AuditChainStatus::HashMismatch);
    }

    TEST(AuditChainVerifierTest, BrokenLinkIsDetected) {
      std::vector<core::AuditEvent> events = {make_event(1), make_event(2), make_event(3)};
      chain(events);
      // Sever the link into row 2 (e.g. a row was deleted/reordered).
      events[2].prev_hash = std::string(64, 'f');
      const auto report = verify_audit_chain(events);
      ASSERT_FALSE(report.ok);
      ASSERT_TRUE(report.first_error.has_value());
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access): guarded by ASSERT_TRUE above
      const auto& err = report.first_error.value();
      EXPECT_EQ(err.index, 2U);
      EXPECT_EQ(err.status, AuditChainStatus::BrokenLink);
    }

    TEST(AuditChainVerifierTest, WrongExpectedFirstPrevIsBrokenLinkAtZero) {
      std::vector<core::AuditEvent> events = {make_event(1), make_event(2)};
      chain(events);
      const auto report = verify_audit_chain(events, std::string(64, 'a'));
      ASSERT_FALSE(report.ok);
      ASSERT_TRUE(report.first_error.has_value());
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access): guarded by ASSERT_TRUE above
      const auto& err = report.first_error.value();
      EXPECT_EQ(err.index, 0U);
      EXPECT_EQ(err.status, AuditChainStatus::BrokenLink);
    }

    // ---- Break-it: large chains and boundary conditions ----

    TEST(AuditChainVerifierTest, LargeChainVerifiesCompletely) {
      std::vector<core::AuditEvent> events;
      events.reserve(1'000);
      for (std::uint64_t i = 1; i <= 1'000; ++i) {
        events.push_back(make_event(i));
      }
      chain(events);
      const auto report = verify_audit_chain(events);
      EXPECT_TRUE(report.ok);
      EXPECT_EQ(report.verified_count, 1'000U);
    }

    TEST(AuditChainVerifierTest, TamperedMiddleOfLargeChainDetected) {
      std::vector<core::AuditEvent> events;
      events.reserve(500);
      for (std::uint64_t i = 1; i <= 500; ++i) {
        events.push_back(make_event(i));
      }
      chain(events);
      // Tamper row 250 — all rows after it become unverifiable.
      events[250].action = "malicious";
      const auto report = verify_audit_chain(events);
      ASSERT_FALSE(report.ok);
      ASSERT_TRUE(report.first_error.has_value());
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
      const auto& err = report.first_error.value();
      EXPECT_EQ(err.index, 250U);
      EXPECT_EQ(err.status, AuditChainStatus::HashMismatch);
    }

    TEST(AuditChainVerifierTest, TamperedFirstRowBreaksEntireChain) {
      std::vector<core::AuditEvent> events = {make_event(1), make_event(2), make_event(3)};
      chain(events);
      events[0].after_json = R"({"name":"evil"})";
      const auto report = verify_audit_chain(events);
      ASSERT_FALSE(report.ok);
      ASSERT_TRUE(report.first_error.has_value());
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
      const auto& err = report.first_error.value();
      EXPECT_EQ(err.index, 0U);
      EXPECT_EQ(err.status, AuditChainStatus::HashMismatch);
      // Because the first hash is wrong, verified_count should be 0.
      EXPECT_EQ(report.verified_count, 0U);
    }

    TEST(AuditChainVerifierTest, VeryLongContentFieldDoesNotCrash) {
      core::AuditEvent event = make_event(1);
      event.before_json = std::string(100'000, 'x');
      event.after_json = std::string(100'000, 'y');
      std::vector<core::AuditEvent> events = {std::move(event)};
      chain(events);
      const auto report = verify_audit_chain(events);
      EXPECT_TRUE(report.ok);
      EXPECT_EQ(report.verified_count, 1U);
    }

    TEST(AuditChainVerifierTest, SingleRowWithWrongExpectedPrevHashFails) {
      std::vector<core::AuditEvent> events = {make_event(1)};
      chain(events);
      // Provide a wrong expected_prev_hash for the 1-element chain.
      const auto report = verify_audit_chain(events, std::string(64, 'f'));
      ASSERT_FALSE(report.ok);
      ASSERT_TRUE(report.first_error.has_value());
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
      const auto& err = report.first_error.value();
      EXPECT_EQ(err.index, 0U);
      EXPECT_EQ(err.status, AuditChainStatus::BrokenLink);
    }

    // ==== Aggressive: zero hashes, duplicate ids, huge chain ====

    TEST(AuditChainVerifierTest, AllZeroHashesChainFails) {
      // Every row has "this_hash" and "prev_hash" as all zeros — impossible for a
      // valid chain. The verifier should detect the break at row 1.
      std::vector<core::AuditEvent> events = {make_event(1), make_event(2)};
      for (auto& e : events) {
        e.prev_hash = std::string(64, '0');
        e.this_hash = std::string(64, '0');
      }
      const auto report = verify_audit_chain(events, std::string(64, '0'));
      ASSERT_FALSE(report.ok);
    }

    TEST(AuditChainVerifierTest, TenThousandRowChainVerifies) {
      std::vector<core::AuditEvent> events;
      events.reserve(10'000);
      for (std::uint64_t i = 1; i <= 10'000; ++i) {
        events.push_back(make_event(i));
      }
      chain(events);
      const auto report = verify_audit_chain(events);
      EXPECT_TRUE(report.ok);
      EXPECT_EQ(report.verified_count, 10'000U);
    }

    TEST(AuditChainVerifierTest, EmptyPrevHashOnFirstRowAccepted) {
      // An empty prev_hash on the first row is equivalent to the zero hash.
      std::vector<core::AuditEvent> events = {make_event(1)};
      events[0].prev_hash.clear();
      events[0].this_hash = compute_audit_hash("", audit_event_content_json(events[0]));
      const auto report = verify_audit_chain(events, "");
      EXPECT_TRUE(report.ok);
    }

    TEST(AuditChainVerifierTest, HashMismatchOnLastRowOfLargeChain) {
      std::vector<core::AuditEvent> events;
      events.reserve(500);
      for (std::uint64_t i = 1; i <= 500; ++i) {
        events.push_back(make_event(i));
      }
      chain(events);
      // Only tamper the very last row.
      events[499].after_json = R"({"name":"hacked"})";
      const auto report = verify_audit_chain(events);
      ASSERT_FALSE(report.ok);
      ASSERT_TRUE(report.first_error.has_value());
      EXPECT_EQ(report.first_error->index, 499U);
      // All preceding rows verified before hitting the broken one.
      EXPECT_EQ(report.verified_count, 499U);
    }

  } // namespace
} // namespace fmgr::audit
