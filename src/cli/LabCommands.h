// SPDX-License-Identifier: AGPL-3.0-or-later

// `freezerctl lab create` — the first-run bootstrap (PRD §16 first-run wizard,
// D1.3). Provisions a brand-new Lab together with its first SystemAdmin User and
// a SystemAdmin LabMembership in a single transaction, so a fresh deployment has
// an administrator and a lab without hand-seeding the database. Password
// enrolment, TOTP, and KMS-wrapped secrets are intentionally deferred to the
// auth-CLI slice; this command mints identity rows only.
#ifndef FMGR_CLI_LABCOMMANDS_H
#define FMGR_CLI_LABCOMMANDS_H

#include "core/ids.h"
#include "storage/IStorageBackend.h"

#include <ostream>
#include <string>

namespace fmgr::cli {

  // Inputs for `lab create`. ids are server-generated (CSPRNG v4), never caller
  // supplied, so the bootstrap cannot collide with or impersonate an existing
  // lab/user.
  struct LabCreateOptions {
    std::string name;
    std::string contact;
    std::string admin_email;
    std::string admin_display_name;
    bool phi_enabled{false};
  };

  // Generated identities, returned so callers/tests need not scrape stdout.
  struct LabCreateResult {
    core::LabId lab_id;
    core::UserId admin_user_id;
  };

  // Provision a lab + first SystemAdmin user + membership atomically. The three
  // rows commit together or not at all; each mutation appends an audit row inside
  // the same transaction. Prints the generated ids to `out` and returns 0 on
  // success. Backend/validation failures propagate as exceptions to run_cli's
  // handler.
  LabCreateResult run_lab_create(storage::IStorageBackend& backend, const LabCreateOptions& options,
                                 std::ostream& out);

} // namespace fmgr::cli

#endif // FMGR_CLI_LABCOMMANDS_H
