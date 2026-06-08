// SPDX-License-Identifier: AGPL-3.0-or-later

// Read-only sample listing for the CLI. Scopes every query to a single lab and
// injects the Postgres RLS session variable so the lab filter is enforced both
// in the predicate and at the row-security layer (no-op on SQLite).
#ifndef FMGR_CLI_SAMPLEQUERY_H
#define FMGR_CLI_SAMPLEQUERY_H

#include "core/ids.h"
#include "core/sample.h"
#include "storage/IStorageBackend.h"

#include <cstddef>
#include <optional>
#include <vector>

namespace fmgr::cli {

  struct SampleQueryOptions {
    core::LabId lab_id;
    std::optional<std::size_t> limit;
    bool include_tombstoned{false};
  };

  // Samples in the given lab, ordered by created_at ascending. Tombstoned rows
  // are excluded unless options.include_tombstoned is set.
  [[nodiscard]] std::vector<core::Sample> query_samples(storage::IStorageBackend& backend,
                                                        const SampleQueryOptions& options);

} // namespace fmgr::cli

#endif // FMGR_CLI_SAMPLEQUERY_H
