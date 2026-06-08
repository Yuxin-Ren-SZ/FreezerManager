// SPDX-License-Identifier: AGPL-3.0-or-later

#include "cli/SampleQuery.h"

#include "core/timestamp.h"
#include "storage/SampleTraits.h"

namespace fmgr::cli {

  std::vector<core::Sample> query_samples(storage::IStorageBackend& backend,
                                          const SampleQueryOptions& options) {
    auto txn = backend.begin(storage::IsolationLevel::Serializable);

    // RLS gate: the samples policy filters on app.current_lab_ids. Bare key —
    // PostgresTransaction prepends "app."; SQLite ignores it. Matches the
    // single-lab scope the predicate below enforces.
    txn->set_session_var("current_lab_ids", options.lab_id.to_string());

    auto query =
        storage::Query<core::Sample>::where(
            storage::field<core::Sample, std::string>(core::Sample::Field::LabId) ==
            options.lab_id.to_string())
            .order_by(storage::field<core::Sample, core::Timestamp>(core::Sample::Field::CreatedAt),
                      storage::SortDirection::Ascending);

    if (options.include_tombstoned) {
      query = query.include_tombstoned();
    }
    if (options.limit.has_value()) {
      query = query.limit(options.limit.value());
    }

    auto samples = txn->repo<core::Sample>().query(query);
    txn->commit();
    return samples;
  }

} // namespace fmgr::cli
