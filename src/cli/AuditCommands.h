// SPDX-License-Identifier: AGPL-3.0-or-later

// `freezerctl audit verify` — walks the append-only audit hash chain and reports
// the first divergence (broken link or tampered row). Global (cross-lab); output
// is written to an injected std::ostream for testability.
#ifndef FMGR_CLI_AUDITCOMMANDS_H
#define FMGR_CLI_AUDITCOMMANDS_H

#include "storage/IStorageBackend.h"

#include <ostream>

namespace fmgr::cli {

  struct AuditVerifyOptions {
    // Reserved for future flags (e.g. a checkpoint to verify from). Empty today.
  };

  // Verify the entire audit chain. Returns 0 if intact, 1 on the first
  // divergence (details written to `out`).
  [[nodiscard]] int run_audit_verify(storage::IStorageBackend& backend,
                                     const AuditVerifyOptions& options, std::ostream& out);

} // namespace fmgr::cli

#endif // FMGR_CLI_AUDITCOMMANDS_H
