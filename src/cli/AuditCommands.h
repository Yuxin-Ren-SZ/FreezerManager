// SPDX-License-Identifier: AGPL-3.0-or-later

// `freezerctl audit verify` — walks the append-only audit hash chain and reports
// the first divergence (broken link or tampered row). Global (cross-lab); output
// is written to an injected std::ostream for testability.
#ifndef FMGR_CLI_AUDITCOMMANDS_H
#define FMGR_CLI_AUDITCOMMANDS_H

#include "core/ids.h"
#include "storage/IStorageBackend.h"

#include <optional>
#include <ostream>

namespace fmgr::cli {

  struct AuditVerifyOptions {
    // Reserved for future flags (e.g. a checkpoint to verify from). Empty today.
  };

  // Verify the entire audit chain. Returns 0 if intact, 1 on the first
  // divergence (details written to `out`).
  [[nodiscard]] int run_audit_verify(storage::IStorageBackend& backend,
                                     const AuditVerifyOptions& options, std::ostream& out);

  struct AuditExportOptions {
    // When set, export only events for this lab (global, lab_id-null events are
    // excluded). When unset, every audit event is exported (global export).
    std::optional<core::LabId> lab_id;
    // Actor recorded on the `audit.export` event that this export itself emits
    // (PRD §7.3: audit export is itself audited).
    core::UserId actor;
  };

  // Export the audit log as chain-of-custody CSV to `out` (PRD §7.3, §13). The
  // export is itself audited: a single `audit.export` event is appended to the
  // chain after the rows are written. Returns 0 on success.
  [[nodiscard]] int run_audit_export(storage::IStorageBackend& backend,
                                     const AuditExportOptions& options, std::ostream& out);

} // namespace fmgr::cli

#endif // FMGR_CLI_AUDITCOMMANDS_H
