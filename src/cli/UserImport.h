// SPDX-License-Identifier: AGPL-3.0-or-later

// Pure CSV-row -> core::User mapping for `freezerctl user import` (admin-only bulk
// identity provisioning, PRD §13). No I/O and no DB: structural validation only
// (required primary_email/display_name; well-formed optional default_lab_id
// UUID). Email uniqueness (case-insensitive) is enforced by the repository
// insert. Users are GLOBAL, not lab-scoped: imported rows are identities with no
// lab access until a membership is granted separately; the import's --lab is only
// the audit/actor context. Server-managed columns (id, status, created_at,
// auth_bindings, totp_secret_enc, authz_version) are ignored if present, so a
// file cannot pre-set a password, MFA secret, or authz epoch.
#ifndef FMGR_CLI_USERIMPORT_H
#define FMGR_CLI_USERIMPORT_H

#include "cli/CsvImport.h"
#include "core/identity.h"
#include "core/ids.h"
#include "core/timestamp.h"

#include <string>
#include <vector>

namespace fmgr::cli {

  // lab_id is unused (users are global); kept for the uniform build_fn signature.
  [[nodiscard]] EntityImportReport<core::User>
  build_user_import(const std::vector<std::vector<std::string>>& records, core::LabId lab_id,
                    core::Timestamp now);

} // namespace fmgr::cli

#endif // FMGR_CLI_USERIMPORT_H
