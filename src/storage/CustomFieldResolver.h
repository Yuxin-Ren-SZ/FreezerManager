// SPDX-License-Identifier: AGPL-3.0-or-later

// Resolve the flat set of sample-scoped CustomFieldDefinitions that apply to a
// given ItemType, merging the lab's global definitions with every definition
// attached to an ancestor in the item-type taxonomy (PRD §4.3). A descendant
// inherits all ancestor fields; on a duplicate `key` the most-derived definition
// wins (a child may tighten validation). The result is the `definitions` argument
// expected by core::validate_custom_fields, which itself performs no resolution.
#ifndef FMGR_STORAGE_CUSTOMFIELDRESOLVER_H
#define FMGR_STORAGE_CUSTOMFIELDRESOLVER_H

#include "core/item_type.h"
#include "storage/IStorageBackend.h"
#include "storage/ItemTypeTraits.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fmgr::storage {

  // Returns the resolved sample-scoped CustomFieldDefinitions for `item_type_id`
  // within `lab_id`, ordered with the lab globals first, then ancestors from root
  // toward the leaf. Archived definitions are excluded (query() filters them).
  // The ancestor walk is cycle-guarded defensively even though the repository
  // rejects lineage cycles.
  [[nodiscard]] inline std::vector<core::CustomFieldDefinition>
  resolve_custom_field_defs(ITransaction& txn, const core::LabId& lab_id,
                            const core::ItemTypeId& item_type_id) {
    auto& item_type_repo = txn.repo<core::ItemType>();

    // Walk leaf -> root, recording each node's specificity rank (leaf highest).
    std::unordered_map<std::string, int> ancestor_rank;
    {
      std::vector<std::string> chain;
      std::unordered_set<std::string> seen;
      std::optional<core::ItemTypeId> cursor = item_type_id;
      while (cursor.has_value()) {
        // The while-condition guarantees a value; the checker loses this across
        // the loop back-edge (cursor is reassigned at the tail).
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        const auto current_id = cursor.value();
        if (!seen.insert(current_id.to_string()).second) {
          break; // cycle guard
        }
        const auto node = item_type_repo.find_by_id(current_id);
        if (!node.has_value()) {
          break;
        }
        chain.push_back(node.value().id.to_string());
        cursor = node.value().parent_id;
      }
      // chain[0] is the leaf; assign descending ranks so the leaf is most specific.
      const int count = static_cast<int>(chain.size());
      for (int i = 0; i < count; ++i) {
        ancestor_rank.emplace(chain[static_cast<std::size_t>(i)], count - i);
      }
    }

    const auto all =
        txn.repo<core::CustomFieldDefinition>().query(Query<core::CustomFieldDefinition>::where(
            field<core::CustomFieldDefinition, std::string>(
                core::CustomFieldDefinition::Field::LabId) == lab_id.to_string()));

    // Best definition per key: highest specificity rank wins (global rank 0).
    std::unordered_map<std::string, std::pair<int, core::CustomFieldDefinition>> best;
    for (const auto& cfd : all) {
      if (cfd.scope_kind != core::ScopeKind::Sample) {
        continue;
      }
      int rank = 0; // global (item_type_id == nullopt)
      if (cfd.item_type_id.has_value()) {
        const auto found = ancestor_rank.find(cfd.item_type_id->to_string());
        if (found == ancestor_rank.end()) {
          continue; // attached to an item type outside this lineage
        }
        rank = found->second;
      }
      const auto slot = best.find(cfd.key);
      if (slot == best.end() || rank >= slot->second.first) {
        best.insert_or_assign(cfd.key, std::pair{rank, cfd});
      }
    }

    std::vector<core::CustomFieldDefinition> resolved;
    resolved.reserve(best.size());
    for (auto& [key, ranked] : best) {
      resolved.push_back(std::move(ranked.second));
    }
    return resolved;
  }

} // namespace fmgr::storage

#endif // FMGR_STORAGE_CUSTOMFIELDRESOLVER_H
