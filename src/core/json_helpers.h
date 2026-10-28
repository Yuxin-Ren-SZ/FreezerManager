// SPDX-License-Identifier: AGPL-3.0-or-later

// Shared helpers for serializing std::optional<T> as JSON null (absent value)
// rather than omitting the key entirely, so consumers can rely on every
// documented key being present in the serialized object.
#ifndef FMGR_CORE_JSON_HELPERS_H
#define FMGR_CORE_JSON_HELPERS_H

#include <nlohmann/json.hpp>

#include <optional>

namespace fmgr::core::json_helpers {

  template <typename Value>
  [[nodiscard]] inline nlohmann::json opt_to_json(const std::optional<Value>& value) {
    if (!value.has_value()) {
      return nullptr;
    }
    return nlohmann::json(value.value());
  }

  template <typename Value>
  [[nodiscard]] inline std::optional<Value> opt_from_json(const nlohmann::json& json) {
    if (json.is_null()) {
      return std::nullopt;
    }
    return json.get<Value>();
  }

  // For keys added after rows were first serialized (e.g. audit snapshots written
  // before a migration): a missing key reads as an absent value, like null.
  template <typename Value>
  [[nodiscard]] inline std::optional<Value> opt_from_json_key(const nlohmann::json& json,
                                                              const char* key) {
    const auto found = json.find(key);
    if (found == json.end()) {
      return std::nullopt;
    }
    return opt_from_json<Value>(*found);
  }

} // namespace fmgr::core::json_helpers

#endif
