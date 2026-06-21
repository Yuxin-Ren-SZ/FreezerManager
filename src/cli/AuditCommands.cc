// SPDX-License-Identifier: AGPL-3.0-or-later

#include "cli/AuditCommands.h"

#include "audit/AuditChainVerifier.h"
#include "cli/AuditCsv.h"
#include "core/audit_event.h"
#include "core/timestamp.h"
#include "storage/AuditTraits.h"

#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

namespace fmgr::cli {

  namespace {

    [[nodiscard]] std::string_view status_label(audit::AuditChainStatus status) {
      switch (status) {
      case audit::AuditChainStatus::BrokenLink:
        return "broken link";
      case audit::AuditChainStatus::HashMismatch:
        return "hash mismatch (row tampered)";
      case audit::AuditChainStatus::Ok:
        return "ok";
      }
      return "unknown";
    }

    // Current wall-clock time as an RFC 3339 / ISO 8601 UTC string for the export
    // header. Seconds precision is plenty for chain-of-custody metadata.
    [[nodiscard]] std::string now_iso8601_utc() {
      const auto now = std::chrono::system_clock::now();
      const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
      std::tm utc{};
#ifdef _WIN32
      gmtime_s(&utc, &seconds);
#else
      gmtime_r(&seconds, &utc);
#endif
      std::array<char, sizeof("2026-06-21T12:34:56Z")> buffer{};
      std::strftime(buffer.data(), buffer.size(), "%Y-%m-%dT%H:%M:%SZ", &utc);
      return {buffer.data()};
    }

    // Read the whole chain in verifier order. Ordering is independent of the
    // hash links, but a stable order keeps the export reproducible.
    [[nodiscard]] std::vector<core::AuditEvent>
    read_ordered_events(storage::IStorageBackend& backend) {
      auto txn = backend.begin(storage::IsolationLevel::Serializable);
      auto events = txn->repo<core::AuditEvent>().query(
          storage::Query<core::AuditEvent>::all()
              .order_by(
                  storage::field<core::AuditEvent, core::Timestamp>(core::AuditEvent::Field::At),
                  storage::SortDirection::Ascending)
              .order_by(storage::field<core::AuditEvent, std::string>(core::AuditEvent::Field::Id),
                        storage::SortDirection::Ascending));
      txn->commit();
      return events;
    }

  } // namespace

  int run_audit_verify(storage::IStorageBackend& backend, const AuditVerifyOptions& /*options*/,
                       std::ostream& out) {
    // Read every audit row (audit_events has no RLS, so this sees all of them).
    // The verifier follows prev->this links, so storage order does not matter;
    // we still sort for stable, readable divergence indices.
    auto txn = backend.begin(storage::IsolationLevel::Serializable);
    const auto events = txn->repo<core::AuditEvent>().query(
        storage::Query<core::AuditEvent>::all()
            .order_by(
                storage::field<core::AuditEvent, core::Timestamp>(core::AuditEvent::Field::At),
                storage::SortDirection::Ascending)
            .order_by(storage::field<core::AuditEvent, std::string>(core::AuditEvent::Field::Id),
                      storage::SortDirection::Ascending));
    txn->commit();

    const auto report = audit::verify_audit_chain(events);
    if (report.ok || !report.first_error.has_value()) {
      out << "OK: " << report.verified_count << " audit event(s) verified\n";
      return 0;
    }

    const auto& error = report.first_error.value();
    out << "DIVERGENCE at #" << error.index << " (id=" << error.id.to_string()
        << "): " << status_label(error.status) << " — " << error.detail << '\n';
    out << report.verified_count << " event(s) verified before the divergence\n";
    return 1;
  }

  int run_audit_export(storage::IStorageBackend& backend, const AuditExportOptions& options,
                       std::ostream& out) {
    auto events = read_ordered_events(backend);

    // Lab scoping is an in-memory filter (mirrors the read-all verify path):
    // global, lab_id-null events are not attributable to one lab, so a
    // lab-scoped export must exclude them and never leak another lab's rows.
    if (options.lab_id.has_value()) {
      std::erase_if(events, [&](const core::AuditEvent& event) {
        return event.lab_id != options.lab_id;
      });
    }

    const std::string lab_filter =
        options.lab_id.has_value() ? options.lab_id->to_string() : std::string("all");
    const std::string exported_at = now_iso8601_utc();
    write_audit_csv(out, events, backend.current_version().value, lab_filter, exported_at);

    // The export is itself audited (PRD §7.3). Append one `audit.export` event
    // recording what was disclosed — row count and scope only, never the rows.
    // ReadCommitted: an audit-only append needs no serialization guarantee.
    auto txn = backend.begin(storage::IsolationLevel::ReadCommitted);
    const storage::MutationContext ctx{
        .actor_user_id = options.actor,
        .actor_session_id = "freezerctl",
        .request_id = "",
        .reason = "audit.export",
        .lab_id = options.lab_id.has_value()
                      ? std::optional<std::string>(options.lab_id->to_string())
                      : std::nullopt,
    };
    const nlohmann::json after = {
        {"lab_filter", lab_filter},
        {"event_count", events.size()},
        {"exported_at", exported_at},
    };
    txn->note_mutation("audit", lab_filter, ctx, "audit.export",
                       storage::AuditSnapshot{.before = std::nullopt, .after = after});
    txn->commit();

    return 0;
  }

} // namespace fmgr::cli
