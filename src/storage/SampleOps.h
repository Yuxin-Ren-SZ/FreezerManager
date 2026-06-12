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
                                  const MutationContext& context) {
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

  // ---- Check-out / check-in / discard ----

  // A single sample lifecycle transition (PRD §4.4). The event id and timestamp
  // are supplied by the caller rather than generated here so the operation is
  // clock-free and deterministically unit-testable (the RPC layer passes a fresh
  // UUID + wall-clock now; tests pass fixed values).
  struct CheckoutCommand {
    core::CheckoutAction action;
    // Quantity consumed on this transition, expressed in any volume unit; it is
    // converted to the sample's unit before subtraction. Ignored when the sample
    // tracks no volume. Only meaningful for CheckedIn (CheckedOut consumes
    // nothing; Destroyed consumes whatever remains).
    std::optional<core::Volume> volume_used;
    std::optional<std::string> reason;
    core::CheckoutEventId event_id;
    core::Timestamp at;
  };

  // Apply `command` to `sample_id` inside the caller's open transaction and append
  // the corresponding immutable CheckoutEvent. Returns the updated sample.
  //
  // Status machine:
  //   CheckedOut: requires Active            -> CheckedOut
  //   CheckedIn:  requires CheckedOut        -> Active (or Depleted when volume
  //               reaches zero); subtracts volume_used from the remaining volume.
  //   Destroyed:  requires Active|CheckedOut -> Destroyed; consumes all remaining
  //               volume.
  //
  // Throws NotFound if the sample does not exist and ConstraintViolation for an
  // illegal transition (already tombstoned/destroyed, or a status that does not
  // permit the requested action). On any throw the caller must roll back.
  inline core::Sample apply_checkout(ITransaction& txn, const core::SampleId& sample_id,
                                     const CheckoutCommand& command,
                                     const MutationContext& context) {
    auto& repo = txn.repo<core::Sample>();
    const auto current = repo.find_by_id(sample_id);
    if (!current.has_value()) {
      throw NotFound("sample not found");
    }

    core::Sample sample = current.value();
    if (sample.status == core::SampleStatus::Tombstoned ||
        sample.status == core::SampleStatus::Destroyed) {
      throw ConstraintViolation("sample is not in a checkout-eligible state");
    }

    // volume_delta records the signed quantity change (negative = consumed), in
    // the sample's own unit, for the chain-of-custody event.
    std::optional<std::int64_t> volume_delta;

    switch (command.action) {
    case core::CheckoutAction::CheckedOut:
      if (sample.status != core::SampleStatus::Active) {
        throw ConstraintViolation("only an active sample can be checked out");
      }
      sample.status = core::SampleStatus::CheckedOut;
      break;

    case core::CheckoutAction::CheckedIn:
      if (sample.status != core::SampleStatus::CheckedOut) {
        throw ConstraintViolation("only a checked-out sample can be checked in");
      }
      sample.status = core::SampleStatus::Active;
      if (command.volume_used.has_value() && sample.volume_value.has_value() &&
          sample.volume_unit.has_value()) {
        const std::int64_t used = command.volume_used->to_unit(*sample.volume_unit).raw_value();
        std::int64_t remaining = *sample.volume_value - used;
        if (remaining < 0) {
          remaining = 0;
        }
        volume_delta = remaining - *sample.volume_value; // negative
        sample.volume_value = remaining;
        if (remaining == 0) {
          sample.status = core::SampleStatus::Depleted; // PRD §4.4 auto-depletion
        }
      }
      break;

    case core::CheckoutAction::Destroyed:
      if (sample.volume_value.has_value()) {
        volume_delta = -*sample.volume_value;
        sample.volume_value = 0;
      }
      sample.status = core::SampleStatus::Destroyed;
      break;
    }

    sample.last_modified_by = context.actor_user_id;
    sample.last_modified_at = command.at;
    repo.update(sample, context);

    const core::CheckoutEvent event{
        .id = command.event_id,
        .sample_id = sample.id,
        .lab_id = sample.lab_id,
        .user_id = context.actor_user_id,
        .action = command.action,
        .reason = command.reason,
        .at = command.at,
        .volume_delta = volume_delta,
        .volume_unit = volume_delta.has_value() ? sample.volume_unit : std::nullopt,
        .location_after = sample.position_label,
    };
    txn.repo<core::CheckoutEvent>().insert(event, context);

    return sample;
  }

} // namespace fmgr::storage

#endif // FMGR_STORAGE_SAMPLEOPS_H
