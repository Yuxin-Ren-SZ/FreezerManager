// SPDX-License-Identifier: AGPL-3.0-or-later

#include "audit/AuditChainVerifier.h"

#include <nlohmann/json.hpp>

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fmgr::audit {

  namespace {
    [[nodiscard]] AuditChainError make_error(std::size_t index, core::AuditEventId id,
                                             AuditChainStatus status, std::string detail) {
      return AuditChainError{
          .index = index, .id = id, .status = status, .detail = std::move(detail)};
    }
  } // namespace

  AuditChainReport verify_audit_chain(const std::vector<core::AuditEvent>& events,
                                      std::string_view expected_first_prev) {
    AuditChainReport report;
    if (events.empty()) {
      return report; // ok, nothing to verify
    }

    // The chain is a linked list embedded in the rows (prev_hash -> this_hash),
    // independent of any storage sort order. Index by this_hash and by prev_hash
    // so we can follow links and detect duplicates/forks.
    std::unordered_map<std::string, std::size_t> by_this;
    std::unordered_map<std::string, std::vector<std::size_t>> by_prev;
    for (std::size_t index = 0; index < events.size(); ++index) {
      const auto& event = events.at(index);
      const auto [iter, inserted] = by_this.emplace(event.this_hash, index);
      if (!inserted) {
        report.ok = false;
        report.first_error = make_error(index, event.id, AuditChainStatus::BrokenLink,
                                        "duplicate this_hash (two rows share the same hash)");
        return report;
      }
      by_prev[event.prev_hash].push_back(index);
    }

    // Locate the head: exactly one row must chain from expected_first_prev.
    const auto head_it = by_prev.find(std::string(expected_first_prev));
    if (head_it == by_prev.end()) {
      const auto& first = events.front();
      report.ok = false;
      report.first_error = make_error(0, first.id, AuditChainStatus::BrokenLink,
                                      "no row chains from the expected starting hash");
      return report;
    }
    if (head_it->second.size() > 1) {
      const auto fork_index = head_it->second.at(1);
      report.ok = false;
      report.first_error =
          make_error(fork_index, events.at(fork_index).id, AuditChainStatus::BrokenLink,
                     "multiple rows share a prev_hash (fork)");
      return report;
    }

    // Walk the chain from the head, recomputing each row's this_hash.
    std::vector<bool> visited(events.size(), false);
    std::size_t current = head_it->second.front();
    while (true) {
      const auto& event = events.at(current);
      if (visited.at(current)) {
        report.ok = false;
        report.first_error =
            make_error(current, event.id, AuditChainStatus::BrokenLink, "cycle detected in chain");
        return report;
      }
      visited.at(current) = true;

      const auto recomputed = compute_audit_hash(event.prev_hash, audit_event_content_json(event));
      if (recomputed != event.this_hash) {
        report.ok = false;
        report.first_error =
            make_error(current, event.id, AuditChainStatus::HashMismatch,
                       "recomputed this_hash does not match the stored value (row tampered)");
        return report;
      }
      ++report.verified_count;

      const auto succ_it = by_prev.find(event.this_hash);
      if (succ_it == by_prev.end()) {
        break; // reached the tail
      }
      if (succ_it->second.size() > 1) {
        const auto fork_index = succ_it->second.at(1);
        report.ok = false;
        report.first_error =
            make_error(fork_index, events.at(fork_index).id, AuditChainStatus::BrokenLink,
                       "multiple rows share a prev_hash (fork)");
        return report;
      }
      current = succ_it->second.front();
    }

    // Every row must lie on the single chain; leftovers are orphans.
    if (report.verified_count != events.size()) {
      for (std::size_t index = 0; index < events.size(); ++index) {
        if (!visited.at(index)) {
          report.ok = false;
          report.first_error = make_error(index, events.at(index).id, AuditChainStatus::BrokenLink,
                                          "row is not connected to the chain (orphan)");
          return report;
        }
      }
    }

    return report;
  }

} // namespace fmgr::audit
