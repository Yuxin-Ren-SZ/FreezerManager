// SPDX-License-Identifier: AGPL-3.0-or-later

#include "cli/NounCommands.h"

#include "cli/EntityRead.h"
#include "core/box.h"
#include "core/freezer.h"
#include "core/item_type.h"
#include "storage/BoxGeometryTraits.h"
#include "storage/FreezerTraits.h"
#include "storage/ItemTypeTraits.h"

#include <optional>
#include <string>

namespace fmgr::cli {

  namespace {

    // Render an optional strong id as its UUID string, or "-" when absent.
    template <typename Id> std::string id_or_dash(const std::optional<Id>& id) {
      return id.has_value() ? id->to_string() : std::string{"-"};
    }

    std::string str_or_dash(const std::optional<std::string>& value) {
      return value.value_or("-");
    }

    std::string temp_or_dash(const std::optional<double>& temp) {
      return temp.has_value() ? std::to_string(temp.value()) : std::string{"-"};
    }

  } // namespace

  void run_freezer_list(storage::IStorageBackend& backend, const NounListOptions& options,
                        std::ostream& out) {
    const auto freezers = query_in_lab<core::Freezer>(backend, options.lab_id, options.limit,
                                                      options.include_tombstoned);
    out << "ID\tNAME\tLOCATION\tMODEL\tTEMP_TARGET_C\n";
    for (const auto& freezer : freezers) {
      out << freezer.id.to_string() << '\t' << freezer.name << '\t' << freezer.location << '\t'
          << freezer.model << '\t' << temp_or_dash(freezer.temp_target_c) << '\n';
    }
    out << freezers.size() << " freezer(s)\n";
  }

  void run_box_list(storage::IStorageBackend& backend, const NounListOptions& options,
                    std::ostream& out) {
    const auto boxes =
        query_in_lab<core::Box>(backend, options.lab_id, options.limit, options.include_tombstoned);
    out << "ID\tLABEL\tBOX_TYPE\tCONTAINER\n";
    for (const auto& box : boxes) {
      out << box.id.to_string() << '\t' << box.label << '\t' << box.box_type_id.to_string() << '\t'
          << box.storage_container_id.to_string() << '\n';
    }
    out << boxes.size() << " box(es)\n";
  }

  void run_item_type_list(storage::IStorageBackend& backend, const NounListOptions& options,
                          std::ostream& out) {
    const auto item_types = query_in_lab<core::ItemType>(backend, options.lab_id, options.limit,
                                                         options.include_tombstoned);
    out << "ID\tNAME\tPARENT\n";
    for (const auto& item_type : item_types) {
      out << item_type.id.to_string() << '\t' << item_type.name << '\t'
          << id_or_dash(item_type.parent_id) << '\n';
    }
    out << item_types.size() << " item-type(s)\n";
  }

  int run_freezer_inspect(storage::IStorageBackend& backend, core::LabId lab_id,
                          const core::FreezerId& id, std::ostream& out) {
    const auto found = find_in_lab<core::Freezer>(backend, lab_id, id);
    if (!found.has_value()) {
      out << "freezer not found in lab: " << id.to_string() << '\n';
      return 1;
    }
    const auto& freezer = found.value();
    out << "id\t" << freezer.id.to_string() << '\n'
        << "lab_id\t" << freezer.lab_id.to_string() << '\n'
        << "name\t" << freezer.name << '\n'
        << "location\t" << freezer.location << '\n'
        << "model\t" << freezer.model << '\n'
        << "temp_target_c\t" << temp_or_dash(freezer.temp_target_c) << '\n'
        << "layout_root_id\t" << freezer.layout_root_id.to_string() << '\n'
        << "created_at\t" << freezer.created_at.unix_micros() << '\n'
        << "archived_at\t"
        << (freezer.archived_at.has_value() ? std::to_string(freezer.archived_at->unix_micros())
                                            : std::string{"-"})
        << '\n';
    return 0;
  }

  int run_box_inspect(storage::IStorageBackend& backend, core::LabId lab_id, const core::BoxId& id,
                      std::ostream& out) {
    const auto found = find_in_lab<core::Box>(backend, lab_id, id);
    if (!found.has_value()) {
      out << "box not found in lab: " << id.to_string() << '\n';
      return 1;
    }
    const auto& box = found.value();
    out << "id\t" << box.id.to_string() << '\n'
        << "lab_id\t" << box.lab_id.to_string() << '\n'
        << "box_type_id\t" << box.box_type_id.to_string() << '\n'
        << "storage_container_id\t" << box.storage_container_id.to_string() << '\n'
        << "label\t" << box.label << '\n'
        << "serial\t" << str_or_dash(box.serial) << '\n'
        << "barcode\t" << str_or_dash(box.barcode) << '\n'
        << "created_at\t" << box.created_at.unix_micros() << '\n'
        << "archived_at\t"
        << (box.archived_at.has_value() ? std::to_string(box.archived_at->unix_micros())
                                        : std::string{"-"})
        << '\n';
    return 0;
  }

  int run_item_type_inspect(storage::IStorageBackend& backend, core::LabId lab_id,
                            const core::ItemTypeId& id, std::ostream& out) {
    const auto found = find_in_lab<core::ItemType>(backend, lab_id, id);
    if (!found.has_value()) {
      out << "item-type not found in lab: " << id.to_string() << '\n';
      return 1;
    }
    const auto& item_type = found.value();
    out << "id\t" << item_type.id.to_string() << '\n'
        << "lab_id\t" << item_type.lab_id.to_string() << '\n'
        << "parent_id\t" << id_or_dash(item_type.parent_id) << '\n'
        << "name\t" << item_type.name << '\n'
        << "created_at\t" << item_type.created_at.unix_micros() << '\n'
        << "archived_at\t"
        << (item_type.archived_at.has_value() ? std::to_string(item_type.archived_at->unix_micros())
                                              : std::string{"-"})
        << '\n';
    return 0;
  }

} // namespace fmgr::cli
