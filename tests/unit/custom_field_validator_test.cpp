// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/custom_field_validator.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>

namespace fmgr::core {
  namespace {

    [[nodiscard]] Uuid uuid_from_low(std::uint64_t low_bits) {
      std::array<std::uint8_t, 16> bytes{};
      for (std::size_t index = 0; index < 8; ++index) {
        bytes.at(15 - index) = static_cast<std::uint8_t>((low_bits >> (index * 8U)) & 0xffU);
      }
      return Uuid(bytes);
    }

    template <typename StrongIdT> [[nodiscard]] StrongIdT id_from_low(std::uint64_t low_bits) {
      return StrongIdT(uuid_from_low(low_bits));
    }

    [[nodiscard]] CustomFieldDefinition make_def(std::string key, FieldDataType data_type,
                                                 bool required = false,
                                                 std::string validation_json = "{}") {
      return CustomFieldDefinition{
          .id = id_from_low<CustomFieldDefinitionId>(1),
          .lab_id = id_from_low<LabId>(2),
          .scope_kind = ScopeKind::Sample,
          .item_type_id = std::nullopt,
          .key = std::move(key),
          .label = "Test Field",
          .data_type = data_type,
          .required = required,
          .validation_json = std::move(validation_json),
          .indexed = false,
          .is_phi = false,
          .created_at = Timestamp::from_unix_micros(1000),
      };
    }

    TEST(CustomFieldValidatorTest, RequiredFieldMissingProducesError) {
      const auto def = make_def("name", FieldDataType::String, /*required=*/true);
      const auto errors = validate_custom_fields(std::span{&def, 1}, nlohmann::json::object());
      ASSERT_EQ(errors.size(), 1U);
      EXPECT_EQ(errors.front().key, "name");
    }

    TEST(CustomFieldValidatorTest, RequiredFieldPresentPassesValidation) {
      const auto def = make_def("name", FieldDataType::String, /*required=*/true);
      const auto fields = nlohmann::json{{"name", "Alice"}};
      const auto errors = validate_custom_fields(std::span{&def, 1}, fields);
      EXPECT_TRUE(errors.empty());
    }

    TEST(CustomFieldValidatorTest, OptionalFieldAbsentPassesValidation) {
      const auto def = make_def("note", FieldDataType::String, /*required=*/false);
      const auto errors = validate_custom_fields(std::span{&def, 1}, nlohmann::json::object());
      EXPECT_TRUE(errors.empty());
    }

    TEST(CustomFieldValidatorTest, TypeMismatchIntFieldGivenStringProducesError) {
      const auto def = make_def("count", FieldDataType::Int);
      const auto fields = nlohmann::json{{"count", "not-a-number"}};
      const auto errors = validate_custom_fields(std::span{&def, 1}, fields);
      ASSERT_EQ(errors.size(), 1U);
      EXPECT_EQ(errors.front().key, "count");
    }

    TEST(CustomFieldValidatorTest, TypeMismatchFloatFieldGivenBoolProducesError) {
      const auto def = make_def("ratio", FieldDataType::Float);
      const auto fields = nlohmann::json{{"ratio", true}};
      const auto errors = validate_custom_fields(std::span{&def, 1}, fields);
      ASSERT_EQ(errors.size(), 1U);
      EXPECT_EQ(errors.front().key, "ratio");
    }

    TEST(CustomFieldValidatorTest, TypeMismatchBoolFieldGivenIntProducesError) {
      const auto def = make_def("active", FieldDataType::Bool);
      const auto fields = nlohmann::json{{"active", 1}};
      const auto errors = validate_custom_fields(std::span{&def, 1}, fields);
      ASSERT_EQ(errors.size(), 1U);
      EXPECT_EQ(errors.front().key, "active");
    }

    TEST(CustomFieldValidatorTest, EnumValueInSetPassesValidation) {
      const auto def =
          make_def("status", FieldDataType::Enum, false, R"({"values":["open","closed"]})");
      const auto fields = nlohmann::json{{"status", "open"}};
      const auto errors = validate_custom_fields(std::span{&def, 1}, fields);
      EXPECT_TRUE(errors.empty());
    }

    TEST(CustomFieldValidatorTest, EnumValueNotInSetProducesError) {
      const auto def =
          make_def("status", FieldDataType::Enum, false, R"({"values":["open","closed"]})");
      const auto fields = nlohmann::json{{"status", "pending"}};
      const auto errors = validate_custom_fields(std::span{&def, 1}, fields);
      ASSERT_EQ(errors.size(), 1U);
      EXPECT_EQ(errors.front().key, "status");
    }

    TEST(CustomFieldValidatorTest, ReferenceFieldValidUuidPassesValidation) {
      const auto def = make_def("parent_sample", FieldDataType::Reference);
      const auto uuid_str = id_from_low<SampleId>(999).to_string();
      const auto fields = nlohmann::json{{"parent_sample", uuid_str}};
      const auto errors = validate_custom_fields(std::span{&def, 1}, fields);
      EXPECT_TRUE(errors.empty());
    }

    TEST(CustomFieldValidatorTest, ReferenceFieldInvalidUuidProducesError) {
      const auto def = make_def("parent_sample", FieldDataType::Reference);
      const auto fields = nlohmann::json{{"parent_sample", "not-a-uuid"}};
      const auto errors = validate_custom_fields(std::span{&def, 1}, fields);
      ASSERT_EQ(errors.size(), 1U);
      EXPECT_EQ(errors.front().key, "parent_sample");
    }

    TEST(CustomFieldValidatorTest, StringMaxLengthEnforcedWhenConfigured) {
      const auto def = make_def("note", FieldDataType::String, false, R"({"max_length":5})");
      const auto fields = nlohmann::json{{"note", "toolong"}};
      const auto errors = validate_custom_fields(std::span{&def, 1}, fields);
      ASSERT_EQ(errors.size(), 1U);
      EXPECT_EQ(errors.front().key, "note");
    }

    TEST(CustomFieldValidatorTest, StringWithinMaxLengthPassesValidation) {
      const auto def = make_def("note", FieldDataType::String, false, R"({"max_length":10})");
      const auto fields = nlohmann::json{{"note", "ok"}};
      const auto errors = validate_custom_fields(std::span{&def, 1}, fields);
      EXPECT_TRUE(errors.empty());
    }

    TEST(CustomFieldValidatorTest, NumericRangeEnforcedWhenConfigured) {
      const auto def = make_def("age", FieldDataType::Int, false, R"({"min":0,"max":150})");
      const auto fields = nlohmann::json{{"age", -1}};
      const auto errors = validate_custom_fields(std::span{&def, 1}, fields);
      ASSERT_EQ(errors.size(), 1U);
      EXPECT_EQ(errors.front().key, "age");
    }

    TEST(CustomFieldValidatorTest, NumericWithinRangePassesValidation) {
      const auto def = make_def("age", FieldDataType::Int, false, R"({"min":0,"max":150})");
      const auto fields = nlohmann::json{{"age", 42}};
      const auto errors = validate_custom_fields(std::span{&def, 1}, fields);
      EXPECT_TRUE(errors.empty());
    }

    TEST(CustomFieldValidatorTest, UnknownExtraFieldsIgnored) {
      const auto def = make_def("name", FieldDataType::String);
      const auto fields = nlohmann::json{{"name", "Alice"}, {"unknown_extra", 999}};
      const auto errors = validate_custom_fields(std::span{&def, 1}, fields);
      EXPECT_TRUE(errors.empty());
    }

    TEST(CustomFieldValidatorTest, MultipleErrorsAccumulated) {
      const std::array defs = {
          make_def("name", FieldDataType::String, /*required=*/true),
          make_def("age", FieldDataType::Int, /*required=*/true),
      };
      // Both fields absent
      const auto errors = validate_custom_fields(defs, nlohmann::json::object());
      EXPECT_EQ(errors.size(), 2U);
    }

    TEST(CustomFieldValidatorTest, DateFieldAcceptsIso8601String) {
      const auto def = make_def("dob", FieldDataType::Date);
      const auto fields = nlohmann::json{{"dob", "2024-01-15"}};
      const auto errors = validate_custom_fields(std::span{&def, 1}, fields);
      EXPECT_TRUE(errors.empty());
    }

    TEST(CustomFieldValidatorTest, DateFieldRejectsNonString) {
      const auto def = make_def("dob", FieldDataType::Date);
      const auto fields = nlohmann::json{{"dob", 2024}};
      const auto errors = validate_custom_fields(std::span{&def, 1}, fields);
      ASSERT_EQ(errors.size(), 1U);
      EXPECT_EQ(errors.front().key, "dob");
    }

    TEST(CustomFieldValidatorTest, DateFieldRejectsBadFormat) {
      const auto def = make_def("dob", FieldDataType::Date);
      const auto fields = nlohmann::json{{"dob", "next Tuesday"}};
      const auto errors = validate_custom_fields(std::span{&def, 1}, fields);
      ASSERT_EQ(errors.size(), 1U);
      EXPECT_EQ(errors.front().key, "dob");
    }

    TEST(CustomFieldValidatorTest, DateFieldRejectsWrongSeparators) {
      const auto def = make_def("dob", FieldDataType::Date);
      const auto fields = nlohmann::json{{"dob", "2024/01/15"}};
      const auto errors = validate_custom_fields(std::span{&def, 1}, fields);
      EXPECT_FALSE(errors.empty());
    }

    TEST(CustomFieldValidatorTest, DateFieldRejectsTooShort) {
      const auto def = make_def("dob", FieldDataType::Date);
      const auto fields = nlohmann::json{{"dob", "2024-01"}};
      const auto errors = validate_custom_fields(std::span{&def, 1}, fields);
      EXPECT_FALSE(errors.empty());
    }

    TEST(CustomFieldValidatorTest, DateFieldRespectsMinMaxFromValidationJson) {
      const auto def = make_def("dob", FieldDataType::Date, false,
                                R"({"min":"2025-01-01","max":"2025-12-31"})");
      // Before min
      auto errors =
          validate_custom_fields(std::span{&def, 1}, nlohmann::json{{"dob", "2024-12-31"}});
      EXPECT_FALSE(errors.empty());
      // After max
      errors = validate_custom_fields(std::span{&def, 1}, nlohmann::json{{"dob", "2026-01-01"}});
      EXPECT_FALSE(errors.empty());
      // Within range
      errors = validate_custom_fields(std::span{&def, 1}, nlohmann::json{{"dob", "2025-06-15"}});
      EXPECT_TRUE(errors.empty());
    }

    TEST(CustomFieldValidatorTest, DatetimeFieldAcceptsIso8601String) {
      const auto def = make_def("updated_at", FieldDataType::Datetime);
      const auto fields = nlohmann::json{{"updated_at", "2024-01-15T10:30:00Z"}};
      const auto errors = validate_custom_fields(std::span{&def, 1}, fields);
      EXPECT_TRUE(errors.empty());
    }

    TEST(CustomFieldValidatorTest, DatetimeFieldRejectsNonString) {
      const auto def = make_def("updated_at", FieldDataType::Datetime);
      const auto fields = nlohmann::json{{"updated_at", 123456}};
      const auto errors = validate_custom_fields(std::span{&def, 1}, fields);
      ASSERT_EQ(errors.size(), 1U);
    }

    TEST(CustomFieldValidatorTest, DatetimeFieldRejectsBadFormat) {
      const auto def = make_def("updated_at", FieldDataType::Datetime);
      const auto fields = nlohmann::json{{"updated_at", "tomorrow morning"}};
      const auto errors = validate_custom_fields(std::span{&def, 1}, fields);
      EXPECT_FALSE(errors.empty());
    }

    TEST(CustomFieldValidatorTest, DatetimeFieldRejectsTooShort) {
      const auto def = make_def("updated_at", FieldDataType::Datetime);
      const auto fields = nlohmann::json{{"updated_at", "2024-01-15"}};
      const auto errors = validate_custom_fields(std::span{&def, 1}, fields);
      EXPECT_FALSE(errors.empty());
    }

    TEST(CustomFieldValidatorTest, DatetimeFieldRejectsMissingT) {
      const auto def = make_def("updated_at", FieldDataType::Datetime);
      const auto fields = nlohmann::json{{"updated_at", "2024-01-15 10:30:00"}};
      const auto errors = validate_custom_fields(std::span{&def, 1}, fields);
      EXPECT_FALSE(errors.empty());
    }

    TEST(CustomFieldValidatorTest, DatetimeFieldRespectsMinMaxFromValidationJson) {
      const auto def = make_def("updated_at", FieldDataType::Datetime, false,
                                R"({"min":"2025-01-01T00:00:00Z","max":"2025-12-31T23:59:59Z"})");
      // Before min
      auto errors = validate_custom_fields(std::span{&def, 1},
                                           nlohmann::json{{"updated_at", "2024-12-31T23:59:59Z"}});
      EXPECT_FALSE(errors.empty());
      // After max
      errors = validate_custom_fields(std::span{&def, 1},
                                      nlohmann::json{{"updated_at", "2026-01-01T00:00:00Z"}});
      EXPECT_FALSE(errors.empty());
      // Within range
      errors = validate_custom_fields(std::span{&def, 1},
                                      nlohmann::json{{"updated_at", "2025-06-15T12:00:00Z"}});
      EXPECT_TRUE(errors.empty());
    }

    TEST(CustomFieldValidatorTest, FloatFieldExceedsMaxProducesError) {
      const auto def = make_def("score", FieldDataType::Float, false, R"({"max":100.0})");
      const auto fields = nlohmann::json{{"score", 150.0}};
      const auto errors = validate_custom_fields(std::span{&def, 1}, fields);
      ASSERT_EQ(errors.size(), 1U);
      EXPECT_EQ(errors.front().key, "score");
    }

    TEST(CustomFieldValidatorTest, FloatFieldBelowMinProducesError) {
      const auto def = make_def("score", FieldDataType::Float, false, R"({"min":0.0})");
      const auto fields = nlohmann::json{{"score", -5.0}};
      const auto errors = validate_custom_fields(std::span{&def, 1}, fields);
      ASSERT_EQ(errors.size(), 1U);
      EXPECT_EQ(errors.front().key, "score");
    }

    TEST(CustomFieldValidatorTest, BoolFieldGivenNullPassesSilently) {
      const auto def = make_def("active", FieldDataType::Bool);
      const auto fields = nlohmann::json{{"active", nullptr}};
      const auto errors = validate_custom_fields(std::span{&def, 1}, fields);
      EXPECT_TRUE(errors.empty());
    }

  } // namespace
} // namespace fmgr::core
