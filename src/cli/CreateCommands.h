// SPDX-License-Identifier: AGPL-3.0-or-later

// `freezerctl <noun> create` — the write half of the M1 CLI nouns (PRD §19 M1),
// symmetric to the read nouns in NounCommands.h. Each command builds one domain
// entity from parsed flags, server-generates its id (CSPRNG v4), and inserts it
// lab-scoped (RLS-gated) with an audit-bearing MutationContext via
// insert_in_lab() (EntityWrite.h). The owning lab must already exist — bootstrap
// one with `freezerctl lab create`.
//
// Box-type positions are generated as a uniform grid from --rows/--cols/--accepts
// (the common cryobox case); mixed-format boxes are deferred to a future
// JSON-template path.
#ifndef FMGR_CLI_CREATECOMMANDS_H
#define FMGR_CLI_CREATECOMMANDS_H

#include "core/enums.h"
#include "core/ids.h"
#include "storage/IStorageBackend.h"

#include <cstdint>
#include <optional>
#include <ostream>
#include <string>

namespace fmgr::cli {

  // Fields common to every create noun: the target lab and the actor recorded in
  // the audit row.
  struct CreateCommon {
    core::LabId lab_id;
    core::UserId actor;
  };

  struct ItemTypeCreateOptions {
    CreateCommon common;
    std::string name;
    std::optional<core::ItemTypeId> parent_id;
  };

  struct ContainerTypeCreateOptions {
    CreateCommon common;
    std::string name;
    std::string size_class;
    std::string material;
    std::string supplier_sku;
  };

  struct StorageContainerCreateOptions {
    CreateCommon common;
    std::string name;
    std::string label;
    core::ContainerKind kind{core::ContainerKind::Custom};
    std::optional<core::StorageContainerId> parent_id;
    std::int32_t ordering_index{0};
  };

  struct FreezerCreateOptions {
    CreateCommon common;
    std::string name;
    std::string location;
    std::string model;
    std::optional<double> temp_target_c;
    core::StorageContainerId layout_root_id;
  };

  struct BoxTypeCreateOptions {
    CreateCommon common;
    std::string name;
    std::string manufacturer;
    std::string sku;
    std::int32_t rows{0};
    std::int32_t cols{0};
    std::string accepts_size_class; // every generated position accepts this token
  };

  struct BoxCreateOptions {
    CreateCommon common;
    std::string label;
    core::BoxTypeId box_type_id;
    core::StorageContainerId storage_container_id;
    std::optional<std::string> serial;
    std::optional<std::string> barcode;
  };

  // Each create inserts one row and returns its generated id; the id is also
  // printed to `out` as `created <noun> <uuid>`.
  core::ItemTypeId run_item_type_create(storage::IStorageBackend& backend,
                                        const ItemTypeCreateOptions& options, std::ostream& out);
  core::ContainerTypeId run_container_type_create(storage::IStorageBackend& backend,
                                                  const ContainerTypeCreateOptions& options,
                                                  std::ostream& out);
  core::StorageContainerId
  run_storage_container_create(storage::IStorageBackend& backend,
                               const StorageContainerCreateOptions& options, std::ostream& out);
  core::FreezerId run_freezer_create(storage::IStorageBackend& backend,
                                     const FreezerCreateOptions& options, std::ostream& out);
  core::BoxTypeId run_box_type_create(storage::IStorageBackend& backend,
                                      const BoxTypeCreateOptions& options, std::ostream& out);
  core::BoxId run_box_create(storage::IStorageBackend& backend, const BoxCreateOptions& options,
                             std::ostream& out);

} // namespace fmgr::cli

#endif // FMGR_CLI_CREATECOMMANDS_H
