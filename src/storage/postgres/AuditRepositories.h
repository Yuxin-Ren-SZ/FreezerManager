// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_STORAGE_POSTGRES_AUDITREPOSITORIES_H
#define FMGR_STORAGE_POSTGRES_AUDITREPOSITORIES_H

#include "storage/postgres/PostgresBackend.h"

namespace fmgr::storage {

  // Register a read-only AuditEventRepository with the backend. find_by_id() and
  // query() are supported; insert(), update(), and soft_delete() throw
  // UnsupportedOperation. Audit rows are written by PostgresTransaction::commit()
  // as part of the hash-chain mechanism — callers must not insert them manually.
  void register_audit_repositories(PostgresBackend& backend);

} // namespace fmgr::storage

#endif // FMGR_STORAGE_POSTGRES_AUDITREPOSITORIES_H
