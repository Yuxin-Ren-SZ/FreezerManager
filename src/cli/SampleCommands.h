// SPDX-License-Identifier: AGPL-3.0-or-later

// `freezerctl sample list` and `sample export` command logic. Output is written
// to an injected std::ostream so the commands are unit-testable without
// touching stdout. Backend access is read-only.
#ifndef FMGR_CLI_SAMPLECOMMANDS_H
#define FMGR_CLI_SAMPLECOMMANDS_H

#include "cli/SampleQuery.h"
#include "core/ids.h"
#include "storage/IStorageBackend.h"

#include <istream>
#include <ostream>

namespace fmgr::cli {

  // Human-readable table of samples in the lab.
  void run_sample_list(storage::IStorageBackend& backend, const SampleQueryOptions& options,
                       std::ostream& out);

  // Chain-of-custody CSV export of samples in the lab (PRD §13).
  void run_sample_export(storage::IStorageBackend& backend, const SampleQueryOptions& options,
                         std::ostream& out);

  // Transactional CSV import of samples (PRD §13). lab_id and actor are
  // server-supplied, never read from the file. In dry_run mode every row is
  // validated (structurally, then against committed DB state in a rolled-back
  // transaction) and a per-row report is written, but nothing is persisted. In
  // normal mode a clean file is imported all-or-nothing in a single transaction.
  // Returns 0 only when every row is valid (and, in normal mode, committed).
  struct SampleImportOptions {
    core::LabId lab_id;
    core::UserId actor;
    bool dry_run{false};
  };

  int run_sample_import(storage::IStorageBackend& backend, const SampleImportOptions& options,
                        std::istream& input, std::ostream& out);

} // namespace fmgr::cli

#endif // FMGR_CLI_SAMPLECOMMANDS_H
