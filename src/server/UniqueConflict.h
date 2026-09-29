// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FMGR_SERVER_UNIQUECONFLICT_H
#define FMGR_SERVER_UNIQUECONFLICT_H

#include "storage/IStorageBackend.h"

#include <string_view>
#include <utility>

namespace fmgr::server {

  // Run a staged write + commit, replacing the storage layer's uninformative
  // ALREADY_EXISTS message with one that names the field the caller supplied
  // (#123): "custom field 'mrn' is already defined on this item type" instead of
  // "UNIQUE constraint failed: index 'cfd_lab_scope_type_key_unique'".
  //
  // The storage layer knows *that* a unique constraint refused a row, and under
  // PostgreSQL's `detail()` even which values collided, but it cannot write a
  // sentence about the caller's request: SQLite reports an index name and
  // PostgreSQL a constraint name, and process-wide there is no mapping from
  // either to the field the caller typed. Only the handler holds the request, so
  // the sentence is written here, once per entity, from what it was about to
  // insert.
  //
  // `write` must contain the whole write, not just the repository call: a
  // repository may refuse a duplicate while staging — when a composite key *is*
  // the entity id, `stage_insert` finds the existing row — or later, when the
  // pending entity is flushed at commit. Both are the same refusal to the caller,
  // and LabMembership is the first kind while CustomFieldDefinition is the second.
  //
  // The catch is by exception type, not by constraint: if another unique
  // constraint in the same write were to fail (an audit-row hash collision, say),
  // it would be described with this sentence. That is the price of not parsing
  // engine text to tell constraints apart, and the code is unchanged either way.
  //
  // This rewrites a message, it does not check anything: the unique index stays
  // the authority, so a caller racing another writer still gets this message
  // rather than a silent success. The engine's own text is carried along in the
  // rethrown exception so the server log keeps the constraint that fired.
  template <typename Write>
  void commit_or_name_conflict(Write&& write, std::string_view client_message) {
    try {
      std::forward<Write>(write)();
    } catch (const storage::UniqueViolation& conflict) {
      throw storage::UniqueViolation(client_message, conflict.backend_detail());
    }
  }

} // namespace fmgr::server

#endif // FMGR_SERVER_UNIQUECONFLICT_H
