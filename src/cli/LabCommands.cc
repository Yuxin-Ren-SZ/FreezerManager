// SPDX-License-Identifier: AGPL-3.0-or-later

#include "cli/LabCommands.h"

#include "core/identity.h"
#include "core/role.h"
#include "core/timestamp.h"
#include "core/uuid.h"
#include "storage/IdentityTraits.h"

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace fmgr::cli {

  namespace {

    // Wall-clock now as a core::Timestamp (UTC microseconds). Mirrors the helper
    // in SampleCommands.cc; the bootstrap is a one-shot command so a small dup is
    // cheaper than a shared clock dependency.
    [[nodiscard]] core::Timestamp now_timestamp() {
      const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();
      return core::Timestamp::from_unix_micros(static_cast<std::int64_t>(micros));
    }

    [[nodiscard]] storage::MutationContext bootstrap_context(const core::LabId& lab_id,
                                                             const core::UserId& actor) {
      // The provisioning actor is the freshly-minted admin itself: a first-run
      // deployment has no prior identity to attribute the action to. The audit
      // lab is the new lab.
      return storage::MutationContext{
          .actor_user_id = actor,
          .actor_session_id = "freezerctl-lab-create",
          .request_id = "",
          .reason = "first_run_bootstrap",
          .lab_id = lab_id.to_string(),
      };
    }

  } // namespace

  LabCreateResult run_lab_create(storage::IStorageBackend& backend, const LabCreateOptions& options,
                                 std::ostream& out) {
    if (options.name.empty()) {
      throw std::invalid_argument("lab name must not be empty");
    }
    if (options.admin_email.empty()) {
      throw std::invalid_argument("admin email must not be empty");
    }

    const auto lab_id = core::LabId::parse(core::generate_uuid_v4());
    const auto admin_id = core::UserId::parse(core::generate_uuid_v4());
    const auto now = now_timestamp();

    const core::Lab lab{
        .id = lab_id,
        .name = options.name,
        .contact = options.contact,
        .created_at = now,
        .is_phi_enabled = options.phi_enabled,
    };

    // Identity-only admin: empty auth_bindings means password-only with no
    // password set yet. Credential enrolment (Argon2id + TOTP) lands with the
    // auth CLI; until then the row exists so memberships and audit can reference
    // it. default_lab_id points at the lab we are creating.
    const core::User admin{
        .id = admin_id,
        .primary_email = options.admin_email,
        .display_name =
            options.admin_display_name.empty() ? options.admin_email : options.admin_display_name,
        .status = core::UserStatus::Active,
        .created_at = now,
        .default_lab_id = lab_id,
    };

    // SystemAdmin grant: a membership whose role is the seeded built-in
    // SystemAdmin role. resolve_permissions() promotes that role's global-only
    // permissions (e.g. lab.provision) to deployment-wide grants.
    const core::LabMembership membership{
        .user_id = admin_id,
        .lab_id = lab_id,
        .role_id = core::builtin_role_id(core::RoleKind::SystemAdmin),
        .joined_at = now,
    };

    const auto ctx = bootstrap_context(lab_id, admin_id);

    // labs / users / lab_memberships carry no RLS policy (provisioning is a
    // global action), so no current_lab_ids injection is needed. All three rows
    // commit together; any failure rolls the whole bootstrap back.
    auto txn = backend.begin(storage::IsolationLevel::Serializable);
    txn->repo<core::Lab>().insert(lab, ctx);
    txn->repo<core::User>().insert(admin, ctx);
    txn->repo<core::LabMembership>().insert(membership, ctx);
    txn->commit();

    out << "created lab " << lab_id.to_string() << '\n';
    out << "created system admin " << admin_id.to_string() << '\n';
    return LabCreateResult{.lab_id = lab_id, .admin_user_id = admin_id};
  }

} // namespace fmgr::cli
