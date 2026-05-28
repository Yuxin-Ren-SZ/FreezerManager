// SPDX-License-Identifier: AGPL-3.0-or-later

// Validator engine for custom field values.
// `validate_custom_fields` takes a flat, already-resolved set of `CustomFieldDefinition`s
// (the caller must perform ancestor-chain resolution; see D7) and a JSON object of submitted
// field values, and returns a list of validation errors. An empty list means valid.
// This is header-only and has no DB or I/O dependencies.
#ifndef FMGR_CORE_CUSTOM_FIELD_VALIDATOR_H
#define FMGR_CORE_CUSTOM_FIELD_VALIDATOR_H

#include "core/item_type.h"
#include "core/uuid.h"

#include <nlohmann/json.hpp>

#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace fmgr::core {

  struct FieldValidationError {
    std::string key;
    std::string message;
  };

  namespace detail {

    [[nodiscard]] inline nlohmann::json parse_constraints(const std::string& json_str) {
      try {
        return nlohmann::json::parse(json_str);
      } catch (const nlohmann::json::exception&) {
        return nlohmann::json::object();
      }
    }

    inline void validate_string_value(const std::string& key, const nlohmann::json& value,
                                      const std::string& validation_json,
                                      std::vector<FieldValidationError>& errors) {
      if (!value.is_string()) {
        errors.push_back({.key = key, .message = "expected string value"});
        return;
      }
      const auto str = value.get<std::string>();
      const auto constraints = parse_constraints(validation_json);
      if (constraints.contains("max_length") && constraints.at("max_length").is_number_integer()) {
        const auto max_len = constraints.at("max_length").get<std::size_t>();
        if (str.size() > max_len) {
          errors.push_back({.key = key,
                            .message = "string length " + std::to_string(str.size()) +
                                       " exceeds max_length " + std::to_string(max_len)});
        }
      }
    }

    inline void validate_int_value(const std::string& key, const nlohmann::json& value,
                                   const std::string& validation_json,
                                   std::vector<FieldValidationError>& errors) {
      if (!value.is_number_integer()) {
        errors.push_back({.key = key, .message = "expected integer value"});
        return;
      }
      const auto num = value.get<std::int64_t>();
      const auto constraints = parse_constraints(validation_json);
      if (constraints.contains("min") && constraints.at("min").is_number()) {
        if (num < constraints.at("min").get<std::int64_t>()) {
          errors.push_back({.key = key, .message = "value is below minimum"});
        }
      }
      if (constraints.contains("max") && constraints.at("max").is_number()) {
        if (num > constraints.at("max").get<std::int64_t>()) {
          errors.push_back({.key = key, .message = "value exceeds maximum"});
        }
      }
    }

    inline void validate_float_value(const std::string& key, const nlohmann::json& value,
                                     const std::string& validation_json,
                                     std::vector<FieldValidationError>& errors) {
      if (!value.is_number() || value.is_boolean()) {
        errors.push_back({.key = key, .message = "expected numeric value"});
        return;
      }
      const auto num = value.get<double>();
      const auto constraints = parse_constraints(validation_json);
      if (constraints.contains("min") && constraints.at("min").is_number()) {
        if (num < constraints.at("min").get<double>()) {
          errors.push_back({.key = key, .message = "value is below minimum"});
        }
      }
      if (constraints.contains("max") && constraints.at("max").is_number()) {
        if (num > constraints.at("max").get<double>()) {
          errors.push_back({.key = key, .message = "value exceeds maximum"});
        }
      }
    }

    inline void validate_enum_value(const std::string& key, const nlohmann::json& value,
                                    const std::string& validation_json,
                                    std::vector<FieldValidationError>& errors) {
      if (!value.is_string()) {
        errors.push_back({.key = key, .message = "expected string value for enum"});
        return;
      }
      const auto str = value.get<std::string>();
      const auto constraints = parse_constraints(validation_json);
      if (!constraints.contains("values") || !constraints.at("values").is_array()) {
        return;
      }
      for (const auto& allowed : constraints.at("values")) {
        if (allowed.is_string() && allowed.get<std::string>() == str) {
          return;
        }
      }
      errors.push_back(
          {.key = key, .message = "value '" + str + "' is not in the allowed enum set"});
    }

    inline void validate_reference_value(const std::string& key, const nlohmann::json& value,
                                         std::vector<FieldValidationError>& errors) {
      if (!value.is_string()) {
        errors.push_back({.key = key, .message = "expected string UUID for reference"});
        return;
      }
      try {
        Uuid::parse(value.get<std::string>());
      } catch (const std::invalid_argument&) {
        errors.push_back({.key = key, .message = "reference value is not a valid UUID"});
      }
    }

    inline void validate_single_field(const CustomFieldDefinition& def,
                                      const nlohmann::json& fields_json,
                                      std::vector<FieldValidationError>& errors) {
      const auto field_iter = fields_json.find(def.key);
      const bool present = field_iter != fields_json.end() && !field_iter->is_null();

      if (def.required && !present) {
        errors.push_back({.key = def.key, .message = "required field is missing or null"});
        return;
      }
      if (!present) {
        return;
      }

      const auto& value = *field_iter;
      switch (def.data_type) {
      case FieldDataType::String:
        validate_string_value(def.key, value, def.validation_json, errors);
        break;
      case FieldDataType::Int:
        validate_int_value(def.key, value, def.validation_json, errors);
        break;
      case FieldDataType::Float:
        validate_float_value(def.key, value, def.validation_json, errors);
        break;
      case FieldDataType::Bool:
        if (!value.is_boolean()) {
          errors.push_back({.key = def.key, .message = "expected boolean value"});
        }
        break;
      case FieldDataType::Date:
      case FieldDataType::Datetime:
        if (!value.is_string()) {
          errors.push_back(
              {.key = def.key,
               .message = "expected string value for " + std::string(to_string(def.data_type))});
        }
        break;
      case FieldDataType::Enum:
        validate_enum_value(def.key, value, def.validation_json, errors);
        break;
      case FieldDataType::Reference:
        validate_reference_value(def.key, value, errors);
        break;
      }
    }

  } // namespace detail

  // Validate `fields_json` (a JSON object) against `definitions`.
  // `definitions` is the flat resolved set for the entity — callers must merge
  // ancestor definitions before calling (ancestor resolution is deferred to D7).
  // Returns empty vector on success. Accumulates all errors before returning.
  [[nodiscard]] inline std::vector<FieldValidationError>
  validate_custom_fields(std::span<const CustomFieldDefinition> definitions,
                         const nlohmann::json& fields_json) {
    std::vector<FieldValidationError> errors;
    for (const auto& def : definitions) {
      detail::validate_single_field(def, fields_json, errors);
    }
    return errors;
  }

} // namespace fmgr::core

#endif // FMGR_CORE_CUSTOM_FIELD_VALIDATOR_H
