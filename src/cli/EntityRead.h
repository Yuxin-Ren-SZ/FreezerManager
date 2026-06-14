// SPDX-License-Identifier: AGPL-3.0-or-later

// Generic, lab-scoped read helpers shared by the freezer/box/item-type CLI read
// nouns. Factored from src/cli/SampleQuery.cc so every noun enforces the same
// single-lab scope at both the query predicate and the Postgres RLS layer
// (set_session_var is a no-op on SQLite). Header-only and templated on the
// entity T; the calling translation unit supplies the concrete entity header
// and its EntityTraits specialization.
#ifndef FMGR_CLI_ENTITYREAD_H
#define FMGR_CLI_ENTITYREAD_H

#include "core/ids.h"
#include "core/timestamp.h"
#include "storage/IStorageBackend.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace fmgr::cli {

  // All rows of entity T in the given lab, ordered by created_at ascending.
  // Tombstoned rows are excluded unless include_tombstoned is set; limit caps the
  // row count when present. Mirrors query_samples (src/cli/SampleQuery.cc).
  template <typename T>
  [[nodiscard]] std::vector<T> query_in_lab(storage::IStorageBackend& backend, core::LabId lab_id,
                                            std::optional<std::size_t> limit = std::nullopt,
                                            bool include_tombstoned = false) {
    auto txn = backend.begin(storage::IsolationLevel::Serializable);

    // RLS gate: the per-table policy filters on app.current_lab_ids. Bare key —
    // PostgresTransaction prepends "app."; SQLite ignores it. Matches the
    // single-lab scope the predicate below enforces.
    txn->set_session_var("current_lab_ids", lab_id.to_string());

    auto query = storage::Query<T>::where(storage::field<T, std::string>(T::Field::LabId) ==
                                          lab_id.to_string())
                     .order_by(storage::field<T, core::Timestamp>(T::Field::CreatedAt),
                               storage::SortDirection::Ascending);
    if (include_tombstoned) {
      query = query.include_tombstoned();
    }
    if (limit.has_value()) {
      query = query.limit(limit.value());
    }

    auto rows = txn->repo<T>().query(query);
    txn->commit();
    return rows;
  }

  // Find a single entity by id, scoped to the lab. Returns nullopt when the row
  // is absent OR belongs to another lab. The explicit lab_id check is
  // defense-in-depth against cross-lab disclosure via a guessed id — it
  // complements (does not replace) the Postgres RLS policy.
  template <typename T>
  [[nodiscard]] std::optional<T> find_in_lab(storage::IStorageBackend& backend, core::LabId lab_id,
                                             const typename T::Id& id) {
    auto txn = backend.begin(storage::IsolationLevel::Serializable);
    txn->set_session_var("current_lab_ids", lab_id.to_string());
    auto found = txn->repo<T>().find_by_id(id);
    txn->commit();
    if (found.has_value() && found->lab_id != lab_id) {
      return std::nullopt;
    }
    return found;
  }

} // namespace fmgr::cli

#endif // FMGR_CLI_ENTITYREAD_H
