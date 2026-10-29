// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_STORAGE_POSTGRES_AUDITREPOSITORIES_H
#define FMGR_STORAGE_POSTGRES_AUDITREPOSITORIES_H

#include "storage/postgres/PostgresBackend.h"

#include "core/audit_event.h"

#include <vector>

namespace fmgr::storage {

  // Register a read-only AuditEventRepository with the backend. find_by_id() and
  // query() are supported; insert(), update(), and soft_delete() throw
  // UnsupportedOperation. Audit rows are written by PostgresTransaction::commit()
  // as part of the hash-chain mechanism — callers must not insert them manually.
  void register_audit_repositories(PostgresBackend& backend);

  // Migrator-only read on its already exclusively locked transaction. Reuses
  // exactly the repository row mapping; no extra connection or transaction.
  [[nodiscard]] std::vector<core::AuditEvent> read_postgres_audit_chain(pqxx::work& txn);

} // namespace fmgr::storage

#endif // FMGR_STORAGE_POSTGRES_AUDITREPOSITORIES_H
