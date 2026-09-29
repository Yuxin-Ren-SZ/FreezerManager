// SPDX-License-Identifier: AGPL-3.0-or-later

// Tightening rules for inherited custom-field definitions (PRD §4.3, N5).
//
// `custom_field_validator.h` validates *values* against one definition; this
// header compares two definitions of the same key — the less-derived one an
// entity inherits and the more-derived one that shadows it — and reports every
// way the child would accept something the parent refuses.
//
// The rule, the same one the SPA's field form implements
// (`src/web/src/features/item-types/itemTypeModel.ts::tightenViolations`):
//
//   a child may tighten a parent's field but must not drop a required parent
//   field.
//
// It lives here, beside the validator, because it is a property of the
// definitions themselves: two values in, a list of violations out — no storage,
// no protobuf, no I/O, which is what `src/core/` is for (AGENTS.md §5).
// Deciding *which* definition a write shadows is the other half and is not pure:
// it needs the ancestor chain, so it stays in
// `storage/CustomFieldResolver.h::resolve_inherited_custom_field_defs`.
//
// The permissive direction is part of the rule, not an oversight: an optional
// inherited field *may* be made required, and a child may add a brand-new
// required field of its own. `indexed` is deliberately not compared — an index
// is a lookup structure, not a constraint on which values are valid, and L10
// forces `indexed` off when a field becomes PHI, so "index removed" would make
// the PHI rule unsatisfiable.
#ifndef FMGR_CORE_CUSTOM_FIELD_TIGHTENING_H
#define FMGR_CORE_CUSTOM_FIELD_TIGHTENING_H

#include "core/custom_field_validator.h"
#include "core/item_type.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace fmgr::core {

  // One way `child` would accept something `parent` refuses.
  struct TighteningViolation {
    // The field or constraint at fault, e.g. "required" or "max_length".
    std::string constraint;
    // One sentence naming the constraint, for the writer's error message.
    std::string message;
  };

  namespace detail {

    struct DeclaredConstraints {
      std::optional<nlohmann::json> max_length;
      std::optional<nlohmann::json> min;
      std::optional<nlohmann::json> max;
      std::optional<std::vector<std::string>> values;
    };

    // The constraints `custom_field_validator.h` implements, as values. Anything
    // else in the JSON — and anything of the wrong type — is ignored, exactly as
    // the validator ignores it and as the SPA's `parseValidation` does.
    [[nodiscard]] inline DeclaredConstraints declared_constraints(const std::string& json_text) {
      const auto parsed = parse_constraints(json_text);
      DeclaredConstraints declared;
      if (!parsed.is_object()) {
        return declared;
      }
      if (const auto it = parsed.find("max_length"); it != parsed.end() && it->is_number()) {
        declared.max_length = *it;
      }
      if (const auto it = parsed.find("min");
          it != parsed.end() && (it->is_number() || it->is_string())) {
        declared.min = *it;
      }
      if (const auto it = parsed.find("max");
          it != parsed.end() && (it->is_number() || it->is_string())) {
        declared.max = *it;
      }
      if (const auto it = parsed.find("values"); it != parsed.end() && it->is_array()) {
        std::vector<std::string> values;
        for (const auto& value : *it) {
          if (!value.is_string()) {
            // A set the validator would ignore is not a constraint to tighten.
            values.clear();
            break;
          }
          values.push_back(value.get<std::string>());
        }
        if (!values.empty()) {
          declared.values = std::move(values);
        }
      }
      return declared;
    }

    // Numeric when both ends are numbers, lexicographic otherwise — `min`/`max`
    // are numbers for int/float and ISO-8601 strings for date/datetime, and the
    // validator compares them the same way. Mirrors the SPA's `compareBound`.
    [[nodiscard]] inline int compare_bound(const nlohmann::json& left,
                                           const nlohmann::json& right) {
      if (left.is_number() && right.is_number()) {
        const auto a = left.get<double>();
        const auto b = right.get<double>();
        return a == b ? 0 : (a < b ? -1 : 1);
      }
      const auto a = left.is_string() ? left.get<std::string>() : left.dump();
      const auto b = right.is_string() ? right.get<std::string>() : right.dump();
      return a == b ? 0 : (a < b ? -1 : 1);
    }

    inline void push_violation(std::vector<TighteningViolation>& violations,
                               const std::string& constraint, const std::string& message) {
      violations.push_back({.constraint = constraint, .message = message});
    }

    inline void compare_constraints(const DeclaredConstraints& parent,
                                    const DeclaredConstraints& child,
                                    std::vector<TighteningViolation>& violations) {
      if (parent.max_length.has_value()) {
        if (!child.max_length.has_value()) {
          push_violation(violations, "max_length",
                         "max_length: the inherited limit of " + parent.max_length->dump() +
                             " may not be dropped");
        } else if (child.max_length->get<double>() > parent.max_length->get<double>()) {
          push_violation(violations, "max_length",
                         "max_length: " + child.max_length->dump() +
                             " raises the inherited limit of " + parent.max_length->dump());
        }
      }

      if (parent.min.has_value()) {
        if (!child.min.has_value()) {
          push_violation(violations, "min",
                         "min: the inherited minimum of " + parent.min->dump() +
                             " may not be dropped");
        } else if (compare_bound(*child.min, *parent.min) < 0) {
          push_violation(violations, "min",
                         "min: " + child.min->dump() + " is below the inherited minimum of " +
                             parent.min->dump());
        }
      }
      if (parent.max.has_value()) {
        if (!child.max.has_value()) {
          push_violation(violations, "max",
                         "max: the inherited maximum of " + parent.max->dump() +
                             " may not be dropped");
        } else if (compare_bound(*child.max, *parent.max) > 0) {
          push_violation(violations, "max",
                         "max: " + child.max->dump() + " exceeds the inherited maximum of " +
                             parent.max->dump());
        }
      }

      if (parent.values.has_value()) {
        if (!child.values.has_value()) {
          push_violation(violations, "values",
                         "values: the inherited set of allowed values may not be dropped");
        } else {
          for (const auto& value : *child.values) {
            if (std::find(parent.values->begin(), parent.values->end(), value) ==
                parent.values->end()) {
              push_violation(violations, "values",
                             "values: '" + value +
                                 "' is not in the inherited set of allowed values");
              break;
            }
          }
        }
      }
    }

  } // namespace detail

  // What stops `child` from replacing `parent` for the same key. An empty list
  // means the replacement is a tightening (or identical).
  //
  // `parent` must be the definition `child` shadows — the resolved less-derived
  // one, not merely one of its ancestors; the caller owns that resolution.
  [[nodiscard]] inline std::vector<TighteningViolation>
  tighten_violations(const CustomFieldDefinition& parent, const CustomFieldDefinition& child) {
    std::vector<TighteningViolation> violations;

    if (parent.data_type != child.data_type) {
      detail::push_violation(violations, "data_type",
                             "data_type: the inherited " +
                                 std::string(to_string(parent.data_type)) + " type may not change");
    }
    if (parent.scope_kind != child.scope_kind) {
      detail::push_violation(violations, "scope_kind",
                             "scope_kind: the inherited " +
                                 std::string(to_string(parent.scope_kind)) +
                                 " scope may not change");
    }
    if (parent.required && !child.required) {
      detail::push_violation(violations, "required",
                             "required: a required inherited field may not be made optional");
    }
    if (parent.is_phi && !child.is_phi) {
      detail::push_violation(violations, "is_phi",
                             "is_phi: an inherited PHI field may not be made non-PHI");
    }

    detail::compare_constraints(detail::declared_constraints(parent.validation_json),
                                detail::declared_constraints(child.validation_json), violations);
    return violations;
  }

  // What a subtree loses when `cfd` is taken away from it and nothing else
  // defines that key: an empty list means the definition constrained nothing, so
  // its absence refuses nothing either. The definition's *identity* — its key,
  // data type and scope — is deliberately not part of this: those describe the
  // field, not which values it accepts, and a removal that leaves a subtree with
  // no definition at all is a loosening only when something was being refused.
  //
  // Expressed through `tighten_violations` rather than beside it: the definition
  // that "nothing left behind" stands for is the most permissive one of the same
  // key — optional, non-PHI, no declared constraints — so a removal reports the
  // constraint it drops in the same vocabulary as every other loosening, and the
  // two can never disagree about what a constraint is (#115).
  [[nodiscard]] inline std::vector<TighteningViolation>
  removal_violations(const CustomFieldDefinition& cfd) {
    auto absent = cfd;
    absent.required = false;
    absent.is_phi = false;
    absent.validation_json = "{}";
    return tighten_violations(cfd, absent);
  }

} // namespace fmgr::core

#endif // FMGR_CORE_CUSTOM_FIELD_TIGHTENING_H
