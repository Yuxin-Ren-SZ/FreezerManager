// SPDX-License-Identifier: AGPL-3.0-or-later

// Hierarchical item-type taxonomy and custom field catalog.
// `ItemType` forms an adjacency-list tree per lab (e.g. liquid → blood, liquid → csf).
// `CustomFieldDefinition` attaches typed metadata fields at any node; descendants inherit
// all ancestor definitions and may add new ones or tighten validation — but cannot remove
// a required ancestor field.
#ifndef FMGR_CORE_ITEM_TYPE_H
#define FMGR_CORE_ITEM_TYPE_H

#include "core/ids.h"
#include "core/timestamp.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace fmgr::core {
  namespace detail {

    template <typename Value>
    [[nodiscard]] inline nlohmann::json it_optional_to_json(const std::optional<Value>& value) {
      if (!value.has_value()) {
        return nullptr;
      }
      return value.value();
    }

    template <typename Value>
    [[nodiscard]] inline std::optional<Value> it_optional_from_json(const nlohmann::json& json) {
      if (json.is_null()) {
        return std::nullopt;
      }
      return json.get<Value>();
    }

  } // namespace detail

  // Which entity kind a CustomFieldDefinition applies to.
  enum class ScopeKind : std::uint8_t { Sample, Box, Freezer, Container };

  // Supported data types for custom field values.
  enum class FieldDataType : std::uint8_t {
    String,
    Int,
    Float,
    Bool,
    Date,
    Datetime,
    Enum,
    Reference, // FK to another Sample by UUID string; FK not enforced at storage layer
  };

  [[nodiscard]] inline std::string_view to_string(ScopeKind kind) {
    switch (kind) {
    case ScopeKind::Sample:
      return "sample";
    case ScopeKind::Box:
      return "box";
    case ScopeKind::Freezer:
      return "freezer";
    case ScopeKind::Container:
      return "container";
    }
    throw std::invalid_argument("unknown scope kind");
  }

  [[nodiscard]] inline ScopeKind parse_scope_kind(std::string_view text) {
    if (text == "sample") {
      return ScopeKind::Sample;
    }
    if (text == "box") {
      return ScopeKind::Box;
    }
    if (text == "freezer") {
      return ScopeKind::Freezer;
    }
    if (text == "container") {
      return ScopeKind::Container;
    }
    throw std::invalid_argument("unknown scope kind");
  }

  [[nodiscard]] inline std::string_view to_string(FieldDataType data_type) {
    switch (data_type) {
    case FieldDataType::String:
      return "string";
    case FieldDataType::Int:
      return "int";
    case FieldDataType::Float:
      return "float";
    case FieldDataType::Bool:
      return "bool";
    case FieldDataType::Date:
      return "date";
    case FieldDataType::Datetime:
      return "datetime";
    case FieldDataType::Enum:
      return "enum";
    case FieldDataType::Reference:
      return "reference";
    }
    throw std::invalid_argument("unknown field data type");
  }

  [[nodiscard]] inline FieldDataType parse_field_data_type(std::string_view text) {
    if (text == "string") {
      return FieldDataType::String;
    }
    if (text == "int") {
      return FieldDataType::Int;
    }
    if (text == "float") {
      return FieldDataType::Float;
    }
    if (text == "bool") {
      return FieldDataType::Bool;
    }
    if (text == "date") {
      return FieldDataType::Date;
    }
    if (text == "datetime") {
      return FieldDataType::Datetime;
    }
    if (text == "enum") {
      return FieldDataType::Enum;
    }
    if (text == "reference") {
      return FieldDataType::Reference;
    }
    throw std::invalid_argument("unknown field data type");
  }

  inline void to_json(nlohmann::json& json, ScopeKind kind) {
    json = std::string(to_string(kind));
  }
  inline void from_json(const nlohmann::json& json, ScopeKind& kind) {
    kind = parse_scope_kind(json.get<std::string>());
  }

  inline void to_json(nlohmann::json& json, FieldDataType data_type) {
    json = std::string(to_string(data_type));
  }
  inline void from_json(const nlohmann::json& json, FieldDataType& data_type) {
    data_type = parse_field_data_type(json.get<std::string>());
  }

  // One node in the lab's item-type taxonomy tree. All fields within a lab are
  // visible to lab members; cross-lab visibility requires an approved ShareRequest.
  struct ItemType {
    using Id = ItemTypeId;

    enum class Field : std::uint8_t {
      Id,
      LabId,
      ParentId,
      Name,
      CreatedAt,
      ArchivedAt,
    };

    ItemTypeId id;
    LabId lab_id;
    std::optional<ItemTypeId> parent_id; // null = root node
    std::string name;
    Timestamp created_at;
    std::optional<Timestamp> archived_at;

    friend bool operator==(const ItemType&, const ItemType&) = default;
  };

  // A typed metadata field definition attached to a scope (Sample, Box, etc.)
  // at an optional ItemType node. Descendants inherit ancestor definitions.
  // `is_phi` routes the field through the encryption layer (Section H) and
  // redaction layer; `indexed` enables a JSON-path index on the storage column.
  // PHI fields may never be indexed (see L10.3).
  struct CustomFieldDefinition {
    using Id = CustomFieldDefinitionId;

    enum class Field : std::uint8_t {
      Id,
      LabId,
      ScopeKind,
      ItemTypeId,
      Key,
      Label,
      DataType,
      Required,
      ValidationJson,
      Indexed,
      IsPhi,
      CreatedAt,
      ArchivedAt,
    };

    CustomFieldDefinitionId id;
    LabId lab_id;
    core::ScopeKind scope_kind;
    std::optional<ItemTypeId> item_type_id; // null = global (applies to all types in lab)
    std::string key;                        // machine key, e.g. "patient_id"
    std::string label;                      // display label
    core::FieldDataType data_type;
    bool required{false};
    std::string validation_json{"{}"}; // type-specific constraint JSON, parsed by validator engine
    bool indexed{false};               // triggers C4.3 JSON-path index (deferred)
    bool is_phi{false};                // PHI fields must never be indexed
    Timestamp created_at;
    std::optional<Timestamp> archived_at;

    friend bool operator==(const CustomFieldDefinition&, const CustomFieldDefinition&) = default;
  };

  inline void to_json(nlohmann::json& json, const ItemType& item_type) {
    json = nlohmann::json{
        {"id", item_type.id},
        {"lab_id", item_type.lab_id},
        {"parent_id", detail::it_optional_to_json(item_type.parent_id)},
        {"name", item_type.name},
        {"created_at", item_type.created_at},
        {"archived_at", detail::it_optional_to_json(item_type.archived_at)},
    };
  }

  inline void from_json(const nlohmann::json& json, ItemType& item_type) {
    item_type = ItemType{
        .id = json.at("id").get<ItemTypeId>(),
        .lab_id = json.at("lab_id").get<LabId>(),
        .parent_id = detail::it_optional_from_json<ItemTypeId>(json.at("parent_id")),
        .name = json.at("name").get<std::string>(),
        .created_at = json.at("created_at").get<Timestamp>(),
        .archived_at = detail::it_optional_from_json<Timestamp>(json.at("archived_at")),
    };
  }

  inline void to_json(nlohmann::json& json, const CustomFieldDefinition& cfd) {
    json = nlohmann::json{
        {"id", cfd.id},
        {"lab_id", cfd.lab_id},
        {"scope_kind", cfd.scope_kind},
        {"item_type_id", detail::it_optional_to_json(cfd.item_type_id)},
        {"key", cfd.key},
        {"label", cfd.label},
        {"data_type", cfd.data_type},
        {"required", cfd.required},
        {"validation_json", cfd.validation_json},
        {"indexed", cfd.indexed},
        {"is_phi", cfd.is_phi},
        {"created_at", cfd.created_at},
        {"archived_at", detail::it_optional_to_json(cfd.archived_at)},
    };
  }

  inline void from_json(const nlohmann::json& json, CustomFieldDefinition& cfd) {
    cfd = CustomFieldDefinition{
        .id = json.at("id").get<CustomFieldDefinitionId>(),
        .lab_id = json.at("lab_id").get<LabId>(),
        .scope_kind = json.at("scope_kind").get<ScopeKind>(),
        .item_type_id = detail::it_optional_from_json<ItemTypeId>(json.at("item_type_id")),
        .key = json.at("key").get<std::string>(),
        .label = json.at("label").get<std::string>(),
        .data_type = json.at("data_type").get<FieldDataType>(),
        .required = json.at("required").get<bool>(),
        .validation_json = json.at("validation_json").get<std::string>(),
        .indexed = json.at("indexed").get<bool>(),
        .is_phi = json.at("is_phi").get<bool>(),
        .created_at = json.at("created_at").get<Timestamp>(),
        .archived_at = detail::it_optional_from_json<Timestamp>(json.at("archived_at")),
    };
  }

} // namespace fmgr::core

#endif // FMGR_CORE_ITEM_TYPE_H
