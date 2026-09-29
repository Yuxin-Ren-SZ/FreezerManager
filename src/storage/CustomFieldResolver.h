// SPDX-License-Identifier: AGPL-3.0-or-later

// Resolve the flat set of sample-scoped CustomFieldDefinitions that apply to a
// given ItemType, merging the lab's global definitions with every definition
// attached to an ancestor in the item-type taxonomy (PRD §4.3). A descendant
// inherits all ancestor fields; on a duplicate `key` the most-derived definition
// wins (a child may tighten validation), and two *live* definitions of one key
// cannot be at the same rank because the write paths refuse the second one
// (`cfd_lab_scope_type_key_unique`, #116). The result is the `definitions` argument
// expected by core::validate_custom_fields, which itself performs no resolution.
//
// `resolve_inherited_custom_field_defs` is the same ranking with the node itself
// left out: exactly the definitions a write at that node shadows, which is what
// the tighten-not-loosen rule (`core/custom_field_tightening.h`, #103) compares
// against.
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

  namespace detail {

    // The item type and its ancestors as ids, leaf first, cycle-guarded: the
    // repository rejects lineage cycles, but a resolver walk must still
    // terminate on a malformed one.
    [[nodiscard]] inline std::vector<std::string>
    item_type_lineage(ITransaction& txn, const core::ItemTypeId& item_type_id) {
      auto& item_type_repo = txn.repo<core::ItemType>();
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
      return chain;
    }

    // Specificity rank per id: the last element of `lineage` is the most
    // specific, lab-global definitions rank 0.
    [[nodiscard]] inline std::unordered_map<std::string, int>
    specificity_ranks(const std::vector<std::string>& lineage) {
      std::unordered_map<std::string, int> ranks;
      const int count = static_cast<int>(lineage.size());
      for (int i = 0; i < count; ++i) {
        ranks.emplace(lineage[static_cast<std::size_t>(i)], count - i);
      }
      return ranks;
    }

    // The most-derived sample-scoped definition per key among the lab's rows
    // attached to a ranked item type, or globally.
    //
    // No two candidates can share a rank (#116): the schema's partial unique
    // index `cfd_lab_scope_type_key_unique` (migration 7, the same definition on
    // both backends) makes (lab_id, scope_kind, COALESCE(item_type_id, ''), key)
    // unique among live rows, which is exactly the tuple a same-rank pair would
    // have to share — two lab-globals collide through the '' sentinel, two rows
    // on one node collide outright. Everything else this loop admits differs in
    // `item_type_id`, and every node of a lineage has its own rank, so two rows
    // of one key can only differ in rank. That is the case `rank >=` (and `rank
    // >`) resolves, and why the tie branch is unreachable rather than merely
    // unlikely: the index is the rule, not this comparison.
    //
    // If that index is ever dropped the tie comes back, and it comes back
    // non-deterministically — the row kept would be whichever the query returns
    // last, which can differ between backends and after a vacuum, and a
    // `required` or `is_phi` flag deciding that way is not cosmetic. So the
    // index is pinned from both sides: `tests/backend_conformance/`'s
    // `*CustomFieldUniquenessConformanceTest` (SQLite and PostgreSQL, both
    // insertion orders, lab-global and one-node pairs) and
    // `ItemTypeRepositoryTest.CustomFieldDefinitionUniqueKeyPerLabScopeType`.
    [[nodiscard]] inline std::vector<core::CustomFieldDefinition>
    best_definitions_per_key(ITransaction& txn, const core::LabId& lab_id,
                             const std::unordered_map<std::string, int>& ranks) {
      const auto all =
          txn.repo<core::CustomFieldDefinition>().query(Query<core::CustomFieldDefinition>::where(
              field<core::CustomFieldDefinition, std::string>(
                  core::CustomFieldDefinition::Field::LabId) == lab_id.to_string()));

      std::unordered_map<std::string, std::pair<int, core::CustomFieldDefinition>> best;
      for (const auto& cfd : all) {
        if (cfd.scope_kind != core::ScopeKind::Sample) {
          continue;
        }
        int rank = 0; // global (item_type_id == nullopt)
        if (cfd.item_type_id.has_value()) {
          const auto found = ranks.find(cfd.item_type_id->to_string());
          if (found == ranks.end()) {
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

  } // namespace detail

  // Returns the resolved sample-scoped CustomFieldDefinitions for `item_type_id`
  // within `lab_id`, ordered with the lab globals first, then ancestors from root
  // toward the leaf. Archived definitions are excluded (query() filters them).
  // The ancestor walk is cycle-guarded defensively even though the repository
  // rejects lineage cycles.
  [[nodiscard]] inline std::vector<core::CustomFieldDefinition>
  resolve_custom_field_defs(ITransaction& txn, const core::LabId& lab_id,
                            const core::ItemTypeId& item_type_id) {
    return detail::best_definitions_per_key(
        txn, lab_id, detail::specificity_ranks(detail::item_type_lineage(txn, item_type_id)));
  }

  // What a definition written at `item_type_id` would shadow: the resolver's
  // ranking with the node itself left out, so what remains is exactly what that
  // node inherits — its ancestors' sample-scoped definitions and the lab globals,
  // one per key. This is the definition "a child may tighten but not loosen"
  // (#103) has to be compared against: `resolve_custom_field_defs` on the node
  // returns the node's own row once it defines the key, which is the one thing a
  // parent lookup must not return.
  [[nodiscard]] inline std::vector<core::CustomFieldDefinition>
  resolve_inherited_custom_field_defs(ITransaction& txn, const core::LabId& lab_id,
                                      const core::ItemTypeId& item_type_id) {
    auto lineage = detail::item_type_lineage(txn, item_type_id);
    if (!lineage.empty()) {
      lineage.erase(lineage.begin()); // the node is the child, not a parent
    }
    return detail::best_definitions_per_key(txn, lab_id, detail::specificity_ranks(lineage));
  }

} // namespace fmgr::storage

#endif // FMGR_STORAGE_CUSTOMFIELDRESOLVER_H
