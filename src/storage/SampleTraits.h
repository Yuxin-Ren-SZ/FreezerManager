// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_STORAGE_SAMPLETRAITS_H
#define FMGR_STORAGE_SAMPLETRAITS_H

#include "core/sample.h"
#include "storage/IStorageBackend.h"

#include <string_view>

namespace fmgr::storage {

  template <> struct EntityTraits<core::Sample> {
    using Id = core::Sample::Id;
    using Field = core::Sample::Field;

    [[nodiscard]] static constexpr std::string_view entity_name() {
      return "sample";
    }

    // Sample uses status = tombstoned as the soft-delete marker, not archived_at_micros.
    // This field is provided for documentation; the SampleRepository hand-writes the
    // status != 'tombstoned' predicate rather than using a generic archived_at IS NULL filter.
    [[nodiscard]] static constexpr Field tombstone_field() {
      return Field::Status;
    }
  };

  template <> struct EntityTraits<core::Project> {
    using Id = core::Project::Id;
    using Field = core::Project::Field;

    [[nodiscard]] static constexpr std::string_view entity_name() {
      return "project";
    }

    [[nodiscard]] static constexpr Field tombstone_field() {
      return Field::ArchivedAt;
    }
  };

  template <> struct EntityTraits<core::SampleProject> {
    using Id = core::SampleProjectId;
    using Field = core::SampleProject::Field;

    [[nodiscard]] static constexpr std::string_view entity_name() {
      return "sample_project";
    }

    // SampleProject is hard-deleted; no tombstone column exists.
    // This field is a dummy required by the EntityTraits contract.
    [[nodiscard]] static constexpr Field tombstone_field() {
      return Field::SampleId;
    }
  };

  template <> struct EntityTraits<core::CheckoutEvent> {
    using Id = core::CheckoutEvent::Id;
    using Field = core::CheckoutEvent::Field;

    [[nodiscard]] static constexpr std::string_view entity_name() {
      return "checkout_event";
    }

    // CheckoutEvent is append-only; no tombstone column exists.
    // This field is a dummy required by the EntityTraits contract.
    [[nodiscard]] static constexpr Field tombstone_field() {
      return Field::Id;
    }
  };

} // namespace fmgr::storage

#endif // FMGR_STORAGE_SAMPLETRAITS_H
