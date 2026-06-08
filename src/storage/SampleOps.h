// SPDX-License-Identifier: AGPL-3.0-or-later

// Sample placement operations layered on the public repository API (no SQL).
//
// A Sample owns exactly one position, so relocating it is a single validated
// update of (box_id, position_label): vacating the source slot is implicit, the
// partial unique index on (box_id, position_label) rejects an occupied
// destination, and any failure rolls back with the caller's transaction — i.e.
// the move is atomic (PRD §4.2). Backend-agnostic: works on any IStorageBackend.
#ifndef FMGR_STORAGE_SAMPLEOPS_H
#define FMGR_STORAGE_SAMPLEOPS_H

#include "core/sample.h"
#include "storage/IStorageBackend.h"
#include "storage/SampleTraits.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace fmgr::storage {

  // Relocate `sample_id` to (dest_box, dest_position) within the caller's open
  // transaction. dest_box and dest_position must both be set (place) or both be
  // null (un-place); the repository validates this along with the destination's
  // existence, the box-type position, and container size-class acceptance.
  //
  // Throws NotFound if the sample does not exist, ConstraintViolation for an
  // invalid destination, and UniqueViolation if the destination is already held
  // by an active sample. On any throw the caller must roll the transaction back,
  // leaving both source and destination untouched.
  inline core::Sample move_sample(ITransaction& txn, const core::SampleId& sample_id,
                                  std::optional<core::BoxId> dest_box,
                                  std::optional<std::string> dest_position,
                                  MutationContext context) {
    auto& repo = txn.repo<core::Sample>();
    const auto current = repo.find_by_id(sample_id);
    if (!current.has_value()) {
      throw NotFound("sample not found");
    }

    core::Sample moved = current.value();
    moved.box_id = dest_box;
    moved.position_label = std::move(dest_position);
    moved.last_modified_by = context.actor_user_id;
    const auto now_micros =
        static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                      std::chrono::system_clock::now().time_since_epoch())
                                      .count());
    moved.last_modified_at = core::Timestamp::from_unix_micros(now_micros);

    // The repository derives the before/after audit snapshots from authoritative
    // state during update(); the move no longer supplies them via the context.
    repo.update(moved, context);
    return moved;
  }

} // namespace fmgr::storage

#endif // FMGR_STORAGE_SAMPLEOPS_H
