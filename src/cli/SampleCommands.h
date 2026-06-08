// SPDX-License-Identifier: AGPL-3.0-or-later

// `freezerctl sample list` and `sample export` command logic. Output is written
// to an injected std::ostream so the commands are unit-testable without
// touching stdout. Backend access is read-only.
#ifndef FMGR_CLI_SAMPLECOMMANDS_H
#define FMGR_CLI_SAMPLECOMMANDS_H

#include "cli/SampleQuery.h"
#include "storage/IStorageBackend.h"

#include <ostream>

namespace fmgr::cli {

  // Human-readable table of samples in the lab.
  void run_sample_list(storage::IStorageBackend& backend, const SampleQueryOptions& options,
                       std::ostream& out);

  // Chain-of-custody CSV export of samples in the lab (PRD §13).
  void run_sample_export(storage::IStorageBackend& backend, const SampleQueryOptions& options,
                         std::ostream& out);

} // namespace fmgr::cli

#endif // FMGR_CLI_SAMPLECOMMANDS_H
