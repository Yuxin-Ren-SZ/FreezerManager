// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FMGR_TEST_SUPPORT_REGISTERREPOSITORIES_H
#define FMGR_TEST_SUPPORT_REGISTERREPOSITORIES_H

#include "storage/sqlite/AuditRepositories.h"
#include "storage/sqlite/BoxGeometryRepositories.h"
#include "storage/sqlite/IdentityRepositories.h"
#include "storage/sqlite/ItemTypeRepositories.h"
#include "storage/sqlite/LayoutRepositories.h"
#include "storage/sqlite/LoginAttemptRepositories.h"
#include "storage/sqlite/RoleRepositories.h"
#include "storage/sqlite/SampleRepositories.h"
#include "storage/sqlite/SessionRepositories.h"
#include "storage/sqlite/ShareRequestRepositories.h"
#include "storage/sqlite/SqliteBackend.h"

namespace fmgr::test {

  inline void register_all_sqlite_repositories(storage::SqliteBackend& backend) {
    storage::register_identity_repositories(backend);
    storage::register_role_repositories(backend);
    storage::register_session_repositories(backend);
    storage::register_login_attempt_repositories(backend);
    storage::register_audit_repositories(backend);
    storage::register_box_geometry_repositories(backend);
    storage::register_box_repositories(backend);
    storage::register_item_type_repositories(backend);
    storage::register_layout_repositories(backend);
    storage::register_sample_repositories(backend);
    storage::register_share_request_repositories(backend);
  }

} // namespace fmgr::test

#endif // FMGR_TEST_SUPPORT_REGISTERREPOSITORIES_H
