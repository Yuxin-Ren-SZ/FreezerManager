// SPDX-License-Identifier: AGPL-3.0-or-later

#include "cli/AuditCommands.h"

#include "audit/AuditChainVerifier.h"
#include "core/audit_event.h"
#include "core/timestamp.h"
#include "storage/AuditTraits.h"

#include <string>
#include <string_view>

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

} // namespace fmgr::cli
