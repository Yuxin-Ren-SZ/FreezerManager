// SPDX-License-Identifier: AGPL-3.0-or-later

// Property tests for the audit hash-chain verifier (PRD §15: audit-chain
// integrity). Events are derived from integers to keep every hashed string
// valid UTF-8 (canonical_json dumps in strict mode).

#include "audit/AuditChainVerifier.h"
#include "audit/CanonicalJson.h"
#include "core/audit_event.h"

#include "test_helpers.h"
#include <gtest/gtest.h>
#include <rapidcheck.h>

#include <string>
#include <vector>

namespace fmgr::audit {
  namespace {
    using namespace fmgr::test;

    [[nodiscard]] core::AuditEvent make_event(std::uint64_t low) {
      core::AuditEvent event;
      event.id = id_from_low<core::AuditEventId>(low);
      event.at = ts(1'000 + static_cast<std::int64_t>(low));
      event.actor_user_id = id_from_low<core::UserId>(low % 7);
      event.actor_session_id = "session-" + std::to_string(low % 3);
      if (low % 2 == 0) {
        event.lab_id = id_from_low<core::LabId>(1 + (low % 4));
      }
      event.action = (low % 3 == 0) ? "insert" : "update";
      event.entity_kind = "sample";
      event.entity_id = id_from_low<core::SampleId>(low).to_string();
      event.before_json = (low % 2 == 0) ? "{}" : R"({"v":1})";
      event.after_json = R"({"v":)" + std::to_string(low) + "}";
      event.request_id = "req-" + std::to_string(low);
      return event;
    }

    [[nodiscard]] std::vector<core::AuditEvent> build_chain(std::size_t count) {
      std::vector<core::AuditEvent> events;
      std::string prev(zero_hash());
      for (std::size_t i = 0; i < count; ++i) {
        auto event = make_event(i + 1);
        event.prev_hash = prev;
        event.this_hash = compute_audit_hash(prev, audit_event_content_json(event));
        prev = event.this_hash;
        events.push_back(std::move(event));
      }
      return events;
    }

    TEST(AuditChainProperty, IntactChainAlwaysVerifies) {
      const bool passed = rc::check("a freshly built chain verifies", [] {
        const auto count = *rc::gen::inRange<std::size_t>(0, 30);
        const auto events = build_chain(count);
        const auto report = verify_audit_chain(events);
        RC_ASSERT(report.ok);
        RC_ASSERT(report.verified_count == count);
      });
      EXPECT_TRUE(passed);
    }

    TEST(AuditChainProperty, TamperingAnyRowIsDetected) {
      const bool passed = rc::check("mutating a hashed field breaks verification", [] {
        const auto count = *rc::gen::inRange<std::size_t>(1, 20);
        auto events = build_chain(count);
        const auto victim = *rc::gen::inRange<std::size_t>(0, count); // [0, count)
        // Mutate a hashed field without recomputing this_hash.
        events.at(victim).action += "-tampered";
        const auto report = verify_audit_chain(events);
        RC_ASSERT(!report.ok);
        RC_ASSERT(report.first_error.has_value());
        RC_ASSERT(report.first_error->status == AuditChainStatus::HashMismatch);
        // Input is in chain order, so the divergence is reported at the victim.
        RC_ASSERT(report.first_error->index == victim);
      });
      EXPECT_TRUE(passed);
    }

  } // namespace
} // namespace fmgr::audit
