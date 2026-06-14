// SPDX-License-Identifier: AGPL-3.0-or-later

// `freezerctl freezer|box|item-type list|inspect` command logic. Read-only;
// output is written to an injected std::ostream so the commands are unit-testable
// without touching stdout. Every read is lab-scoped and RLS-gated via the
// helpers in EntityRead.h.
#ifndef FMGR_CLI_NOUNCOMMANDS_H
#define FMGR_CLI_NOUNCOMMANDS_H

#include "core/ids.h"
#include "storage/IStorageBackend.h"

#include <cstddef>
#include <optional>
#include <ostream>

namespace fmgr::cli {

  // Shared options for every `<noun> list` command.
  struct NounListOptions {
    core::LabId lab_id;
    std::optional<std::size_t> limit;
    bool include_tombstoned{false};
  };

  void run_freezer_list(storage::IStorageBackend& backend, const NounListOptions& options,
                        std::ostream& out);
  void run_box_list(storage::IStorageBackend& backend, const NounListOptions& options,
                    std::ostream& out);
  void run_item_type_list(storage::IStorageBackend& backend, const NounListOptions& options,
                          std::ostream& out);

  // `<noun> inspect`: print a FIELD<TAB>VALUE detail block for the entity, scoped
  // to the lab. Returns 0 when found, 1 when no such entity exists in the lab (an
  // id belonging to another lab is reported as not-found — no cross-lab
  // disclosure). The not-found message is written to out.
  int run_freezer_inspect(storage::IStorageBackend& backend, core::LabId lab_id,
                          const core::FreezerId& id, std::ostream& out);
  int run_box_inspect(storage::IStorageBackend& backend, core::LabId lab_id, const core::BoxId& id,
                      std::ostream& out);
  int run_item_type_inspect(storage::IStorageBackend& backend, core::LabId lab_id,
                            const core::ItemTypeId& id, std::ostream& out);

} // namespace fmgr::cli

#endif // FMGR_CLI_NOUNCOMMANDS_H
