// SPDX-License-Identifier: AGPL-3.0-or-later

// Generic, lab-scoped write helper shared by the freezer/box/item-type/... CLI
// create nouns. The write counterpart to EntityRead.h's query_in_lab: it opens a
// Serializable transaction, injects the Postgres RLS session var so the per-table
// policy admits the insert (no-op on SQLite), inserts the entity with an
// audit-bearing MutationContext, and commits. Header-only and templated on the
// entity T; the calling translation unit supplies the concrete entity header and
// its EntityTraits specialization.
#ifndef FMGR_CLI_ENTITYWRITE_H
#define FMGR_CLI_ENTITYWRITE_H

#include "core/ids.h"
#include "storage/IStorageBackend.h"

namespace fmgr::cli {

  // Insert `entity` into `lab_id`, atomically with its audit row. The RLS gate
  // mirrors query_in_lab: app.current_lab_ids is set to the single target lab so
  // the insert satisfies the lab-scoped policy and cannot land in another lab.
  // Backend/validation failures propagate as exceptions to run_cli's handler.
  template <typename T>
  void insert_in_lab(storage::IStorageBackend& backend, core::LabId lab_id, const T& entity,
                     const storage::MutationContext& ctx) {
    auto txn = backend.begin(storage::IsolationLevel::Serializable);
    txn->set_session_var("current_lab_ids", lab_id.to_string());
    txn->repo<T>().insert(entity, ctx);
    txn->commit();
  }

} // namespace fmgr::cli

#endif // FMGR_CLI_ENTITYWRITE_H
