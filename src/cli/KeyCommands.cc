// SPDX-License-Identifier: AGPL-3.0-or-later

#include "cli/KeyCommands.h"

#include "core/identity.h"
#include "core/sample.h"
#include "crypto/FieldCipher.h"
#include "storage/IdentityTraits.h"
#include "storage/SampleTraits.h"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <optional>
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

    // Re-wrap at most this many samples per transaction. Bounding the batch keeps
    // each Serializable transaction short so rotation does not hold a single
    // long-lived transaction that conflicts with live writes on a large lab
    // (review F-7). A lab with millions of samples rotates over many small txns.
    constexpr std::size_t k_rotation_batch = 100;

    void rotate_lab(storage::IStorageBackend& backend, const kms::IKmsProvider& kms,
                    const core::LabId& lab_id, core::UserId actor, std::ostream& sink,
                    RotationReport& report) {
      // Phase 1: enumerate the PHI-bearing sample IDs in one short read-only txn.
      std::vector<core::SampleId> ids;
      {
        auto txn = backend.begin(storage::IsolationLevel::ReadCommitted);
        txn->set_session_var("current_lab_ids", lab_id.to_string());
        const auto samples = txn->repo<core::Sample>().query(
            storage::Query<core::Sample>::where(
                storage::field<core::Sample, std::string>(core::Sample::Field::LabId) ==
                lab_id.to_string())
                .include_tombstoned());
        txn->commit();
        ids.reserve(samples.size());
        for (const auto& sample : samples) {
          if (has_phi(sample)) {
            ids.push_back(sample.id);
          }
        }
      }

      const storage::MutationContext ctx{
          .actor_user_id = actor,
          .actor_session_id = "freezerctl",
          .request_id = "",
          .reason = "key.rotate",
          .lab_id = lab_id.to_string(),
      };

      // Phase 2: re-wrap in bounded batches, one Serializable txn per batch. Each
      // sample is re-read inside its batch txn so a concurrent edit between
      // enumeration and re-wrap is never clobbered with stale ciphertext.
      for (std::size_t start = 0; start < ids.size(); start += k_rotation_batch) {
        const std::size_t end = std::min(start + k_rotation_batch, ids.size());
        auto txn = backend.begin(storage::IsolationLevel::Serializable);
        txn->set_session_var("current_lab_ids", lab_id.to_string());
        for (std::size_t i = start; i < end; ++i) {
          const auto current = txn->repo<core::Sample>().find_by_id(ids[i]);
          if (!current.has_value() || !has_phi(*current)) {
            continue; // deleted or PHI cleared since enumeration
          }
          ++report.scanned;
          std::optional<std::string> rotated;
          try {
            rotated = crypto::rewrap(current->phi_fields_enc_json, kms);
          } catch (const std::exception& error) {
            ++report.failed;
            sink << "  ! " << current->id.to_string() << ": cannot re-wrap: " << error.what()
                 << " (the KEK that wrapped this record may be missing; ensure the retired key "
                    "files master_kek.prev.* are still present in the credential directory)\n";
            continue;
          }
          if (!rotated.has_value()) {
            ++report.current; // already on the active KEK
            continue;
          }
          // Preserve every field (incl. last_modified_*) except the re-wrapped
          // envelope: rotation is transparent, not a user edit.
          core::Sample updated = *current;
          updated.phi_fields_enc_json = *rotated;
          try {
            txn->repo<core::Sample>().update(updated, ctx);
            ++report.rewrapped;
          } catch (const std::exception& error) {
            ++report.failed;
            sink << "  ! " << current->id.to_string() << ": update rejected: " << error.what()
                 << '\n';
          }
        }
        txn->commit();
      }
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
