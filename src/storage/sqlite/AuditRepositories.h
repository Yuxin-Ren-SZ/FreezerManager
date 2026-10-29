// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_STORAGE_SQLITE_AUDITREPOSITORIES_H
#define FMGR_STORAGE_SQLITE_AUDITREPOSITORIES_H

#include "storage/sqlite/SqliteBackend.h"

#include "core/audit_event.h"

#include <vector>

namespace fmgr::storage {

  // Register a read-only AuditEventRepository with the backend.
  // The repository supports find_by_id() and query(); insert(), update(), and
  // soft_delete() all throw UnsupportedOperation.
  // Audit rows are written automatically by SqliteTransaction::commit() as part
  // of the hash-chain mechanism — callers must not insert them manually.
  void register_audit_repositories(SqliteBackend& backend);

  // Migrator-only read on its already write-locked connection. Reuses exactly
  // the repository row mapping and never starts a second transaction.
  [[nodiscard]] std::vector<core::AuditEvent> read_sqlite_audit_chain(sqlite3* handle);

} // namespace fmgr::storage

#endif // FMGR_STORAGE_SQLITE_AUDITREPOSITORIES_H
