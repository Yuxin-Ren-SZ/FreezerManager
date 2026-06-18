// SPDX-License-Identifier: AGPL-3.0-or-later

#include "cli/KeyCommands.h"

#include "core/identity.h"
#include "core/sample.h"
#include "crypto/FieldCipher.h"
#include "storage/IdentityTraits.h"
#include "storage/SampleTraits.h"

#include <exception>
#include <string>
#include <vector>

namespace fmgr::cli {

  namespace {

    [[nodiscard]] std::vector<core::LabId> target_labs(storage::IStorageBackend& backend,
                                                       std::optional<core::LabId> lab) {
      if (lab.has_value()) {
        return {*lab};
      }
      std::vector<core::LabId> labs;
      auto txn = backend.begin(storage::IsolationLevel::ReadCommitted);
      const auto rows =
          txn->repo<core::Lab>().query(storage::Query<core::Lab>::all().include_tombstoned());
      txn->commit();
      labs.reserve(rows.size());
      for (const auto& row : rows) {
        labs.push_back(row.id);
      }
      return labs;
    }

    [[nodiscard]] bool has_phi(const core::Sample& sample) {
      return !sample.phi_fields_enc_json.empty() && sample.phi_fields_enc_json != "{}";
    }

    void rotate_lab(storage::IStorageBackend& backend, const kms::IKmsProvider& kms,
                    const core::LabId& lab_id, core::UserId actor, std::ostream& sink,
                    RotationReport& report) {
      auto txn = backend.begin(storage::IsolationLevel::Serializable);
      txn->set_session_var("current_lab_ids", lab_id.to_string());
      const auto samples = txn->repo<core::Sample>().query(
          storage::Query<core::Sample>::where(storage::field<core::Sample, std::string>(
                                                  core::Sample::Field::LabId) == lab_id.to_string())
              .include_tombstoned());

      const storage::MutationContext ctx{
          .actor_user_id = actor,
          .actor_session_id = "freezerctl",
          .request_id = "",
          .reason = "key.rotate",
          .lab_id = lab_id.to_string(),
      };

      for (const auto& sample : samples) {
        if (!has_phi(sample)) {
          continue;
        }
        ++report.scanned;
        std::optional<std::string> rotated;
        try {
          rotated = crypto::rewrap(sample.phi_fields_enc_json, kms);
        } catch (const std::exception& error) {
          ++report.failed;
          sink << "  ! " << sample.id.to_string() << ": cannot re-wrap: " << error.what() << '\n';
          continue;
        }
        if (!rotated.has_value()) {
          ++report.current; // already on the active KEK
          continue;
        }
        // Preserve every field (incl. last_modified_*) except the re-wrapped
        // envelope: rotation is transparent, not a user edit.
        core::Sample updated = sample;
        updated.phi_fields_enc_json = *rotated;
        try {
          txn->repo<core::Sample>().update(updated, ctx);
          ++report.rewrapped;
        } catch (const std::exception& error) {
          ++report.failed;
          sink << "  ! " << sample.id.to_string() << ": update rejected: " << error.what() << '\n';
        }
      }
      txn->commit();
    }

  } // namespace

  RotationReport rotate_phi_keys(storage::IStorageBackend& backend, const kms::IKmsProvider& kms,
                                 std::optional<core::LabId> lab, core::UserId actor,
                                 std::ostream& sink) {
    RotationReport report;
    for (const auto& lab_id : target_labs(backend, lab)) {
      rotate_lab(backend, kms, lab_id, actor, sink, report);
    }
    return report;
  }

} // namespace fmgr::cli
