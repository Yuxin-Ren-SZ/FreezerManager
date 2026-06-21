// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/custom_field_validator.h"

#include "test_helpers.h"
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>

namespace fmgr::core {
  namespace {
    using namespace fmgr::test;

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
      const auto def =
          make_def("dob", FieldDataType::Date, false, R"({"min":"2025-01-01","max":"2025-12-31"})");
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

    TEST(CustomFieldValidatorTest, MalformedValidationJsonProducesNoCrash) {
      // parse_constraints catches the json exception and returns empty object.
      // Validation proceeds as if there were no constraints — the value itself
      // must still be type-correct.
      const auto def = make_def("note", FieldDataType::String, false, "not valid json {{{");
      const auto fields = nlohmann::json{{"note", "hello"}};
      const auto errors = validate_custom_fields(std::span{&def, 1}, fields);
      EXPECT_TRUE(errors.empty());
    }

    TEST(CustomFieldValidatorTest, StringFieldGivenNonStringProducesError) {
      const auto def = make_def("name", FieldDataType::String);
      const auto fields = nlohmann::json{{"name", 42}};
      const auto errors = validate_custom_fields(std::span{&def, 1}, fields);
      ASSERT_EQ(errors.size(), 1U);
      EXPECT_EQ(errors.front().key, "name");
    }

    TEST(CustomFieldValidatorTest, EnumFieldGivenNonStringProducesError) {
      const auto def =
          make_def("status", FieldDataType::Enum, false, R"({"values":["open","closed"]})");
      const auto fields = nlohmann::json{{"status", 42}};
      const auto errors = validate_custom_fields(std::span{&def, 1}, fields);
      ASSERT_EQ(errors.size(), 1U);
      EXPECT_EQ(errors.front().key, "status");
    }

    TEST(CustomFieldValidatorTest, EnumFieldWithNoValuesArrayPassesValidation) {
      // No "values" array in constraints → no enum check to perform.
      const auto def = make_def("status", FieldDataType::Enum, false, "{}");
      const auto fields = nlohmann::json{{"status", "anything"}};
      const auto errors = validate_custom_fields(std::span{&def, 1}, fields);
      EXPECT_TRUE(errors.empty());
    }

    TEST(CustomFieldValidatorTest, ReferenceFieldGivenNonStringProducesError) {
      const auto def = make_def("parent_sample", FieldDataType::Reference);
      const auto fields = nlohmann::json{{"parent_sample", 42}};
      const auto errors = validate_custom_fields(std::span{&def, 1}, fields);
      ASSERT_EQ(errors.size(), 1U);
      EXPECT_EQ(errors.front().key, "parent_sample");
    }

    TEST(CustomFieldValidatorTest, IntFieldExceedsMaxProducesError) {
      const auto def = make_def("age", FieldDataType::Int, false, R"({"max":100})");
      const auto fields = nlohmann::json{{"age", 150}};
      const auto errors = validate_custom_fields(std::span{&def, 1}, fields);
      ASSERT_EQ(errors.size(), 1U);
      EXPECT_EQ(errors.front().key, "age");
    }

    // ---- Break-it: null and boundary values ----

    TEST(CustomFieldValidatorTest, NullValueForRequiredFieldProducesError) {
      const auto def = make_def("name", FieldDataType::String, /*required=*/true);
      const auto fields = nlohmann::json{{"name", nullptr}};
      const auto errors = validate_custom_fields(std::span{&def, 1}, fields);
      ASSERT_EQ(errors.size(), 1U);
      EXPECT_EQ(errors.front().key, "name");
    }

    TEST(CustomFieldValidatorTest, EmptyStringForRequiredFieldPasses) {
      // An empty string is a present value — required-ness only checks key existence.
      const auto def = make_def("name", FieldDataType::String, /*required=*/true);
      const auto fields = nlohmann::json{{"name", ""}};
      const auto errors = validate_custom_fields(std::span{&def, 1}, fields);
      EXPECT_TRUE(errors.empty());
    }

    TEST(CustomFieldValidatorTest, IntFieldAtMinBoundaryPasses) {
      const auto def = make_def("age", FieldDataType::Int, false, R"({"min":0})");
      const auto fields = nlohmann::json{{"age", 0}};
      const auto errors = validate_custom_fields(std::span{&def, 1}, fields);
      EXPECT_TRUE(errors.empty());
    }

    TEST(CustomFieldValidatorTest, IntFieldAtMaxBoundaryPasses) {
      const auto def = make_def("age", FieldDataType::Int, false, R"({"max":100})");
      const auto fields = nlohmann::json{{"age", 100}};
      const auto errors = validate_custom_fields(std::span{&def, 1}, fields);
      EXPECT_TRUE(errors.empty());
    }

    TEST(CustomFieldValidatorTest, IntFieldBelowMinProducesError) {
      const auto def = make_def("age", FieldDataType::Int, false, R"({"min":0})");
      const auto fields = nlohmann::json{{"age", -1}};
      const auto errors = validate_custom_fields(std::span{&def, 1}, fields);
      ASSERT_EQ(errors.size(), 1U);
      EXPECT_EQ(errors.front().key, "age");
    }

    // ---- Break-it: extremely long / large values ----

    TEST(CustomFieldValidatorTest, VeryLongStringFieldDoesNotCrash) {
      const auto def = make_def("note", FieldDataType::String);
      const auto fields = nlohmann::json{{"note", std::string(100'000, 'x')}};
      const auto errors = validate_custom_fields(std::span{&def, 1}, fields);
      EXPECT_TRUE(errors.empty());
    }

    TEST(CustomFieldValidatorTest, StringAtMaxLengthBoundaryPasses) {
      const auto def = make_def("note", FieldDataType::String, false, R"({"max_length":5})");
      const auto fields = nlohmann::json{{"note", "12345"}};
      const auto errors = validate_custom_fields(std::span{&def, 1}, fields);
      EXPECT_TRUE(errors.empty());
    }

    TEST(CustomFieldValidatorTest, StringLengthZeroWithMaxLengthPasses) {
      const auto def = make_def("note", FieldDataType::String, false, R"({"max_length":5})");
      const auto fields = nlohmann::json{{"note", ""}};
      const auto errors = validate_custom_fields(std::span{&def, 1}, fields);
      EXPECT_TRUE(errors.empty());
    }

    // ---- Break-it: Unicode and special characters ----

    TEST(CustomFieldValidatorTest, UnicodeStringFieldPasses) {
      const auto def = make_def("note", FieldDataType::String);
      const auto fields = nlohmann::json{{"note", "你好世界🌍"}};
      const auto errors = validate_custom_fields(std::span{&def, 1}, fields);
      EXPECT_TRUE(errors.empty());
    }

    TEST(CustomFieldValidatorTest, UnicodeLengthMeasuredInUtf8Bytes) {
      // max_length=5 in bytes: "é" is 2 bytes in UTF-8, "ééé" is 6 bytes → too long.
      const auto def = make_def("note", FieldDataType::String, false, R"({"max_length":5})");
      const auto fields = nlohmann::json{{"note", "ééé"}};
      const auto errors = validate_custom_fields(std::span{&def, 1}, fields);
      ASSERT_EQ(errors.size(), 1U);
      EXPECT_EQ(errors.front().key, "note");
    }

    // ---- Break-it: extreme numeric values ----

    TEST(CustomFieldValidatorTest, IntFieldVeryLargeValue) {
      const auto def = make_def("count", FieldDataType::Int);
      const auto fields = nlohmann::json{{"count", 9'223'372'036'854'775'807LL}}; // INT64_MAX-ish
      const auto errors = validate_custom_fields(std::span{&def, 1}, fields);
      EXPECT_TRUE(errors.empty());
    }

    TEST(CustomFieldValidatorTest, FloatFieldVeryLargeValue) {
      const auto def = make_def("score", FieldDataType::Float);
      const auto fields = nlohmann::json{{"score", 1e308}};
      const auto errors = validate_custom_fields(std::span{&def, 1}, fields);
      EXPECT_TRUE(errors.empty());
    }

    TEST(CustomFieldValidatorTest, FloatFieldZeroBoundary) {
      const auto def = make_def("score", FieldDataType::Float, false, R"({"min":0.0})");
      const auto fields = nlohmann::json{{"score", 0.0}};
      const auto errors = validate_custom_fields(std::span{&def, 1}, fields);
      EXPECT_TRUE(errors.empty());
    }

    // ---- Break-it: empty / zero-def edge cases ----

    TEST(CustomFieldValidatorTest, EmptyDefinitionsSpanNoCrash) {
      const std::span<const CustomFieldDefinition> empty_defs;
      const auto errors = validate_custom_fields(empty_defs, nlohmann::json::object());
      EXPECT_TRUE(errors.empty());
    }

    TEST(CustomFieldValidatorTest, NullJsonFieldsNoCrash) {
      const auto def = make_def("name", FieldDataType::String, /*required=*/true);
      const auto errors = validate_custom_fields(std::span{&def, 1}, nullptr);
      ASSERT_EQ(errors.size(), 1U);
    }

    TEST(CustomFieldValidatorTest, FieldsWithNestedObjectProducesTypeError) {
      const auto def = make_def("name", FieldDataType::String);
      const auto fields = nlohmann::json{{"name", nlohmann::json::object()}};
      const auto errors = validate_custom_fields(std::span{&def, 1}, fields);
      ASSERT_EQ(errors.size(), 1U);
      EXPECT_EQ(errors.front().key, "name");
    }

    TEST(CustomFieldValidatorTest, FieldsWithArrayProducesTypeError) {
      const auto def = make_def("name", FieldDataType::String);
      const auto fields = nlohmann::json{{"name", nlohmann::json::array()}};
      const auto errors = validate_custom_fields(std::span{&def, 1}, fields);
      ASSERT_EQ(errors.size(), 1U);
      EXPECT_EQ(errors.front().key, "name");
    }

    // ==== Aggressive: injection payloads, deep nesting, contradictory configs ====

    TEST(CustomFieldValidatorTest, InjectionPayloadSqlLikeString) {
      const auto def = make_def("note", FieldDataType::String);
      const auto fields = nlohmann::json{{"note", "'; DROP TABLE samples; --"}};
      const auto errors = validate_custom_fields(std::span{&def, 1}, fields);
      EXPECT_TRUE(errors.empty());
    }

    TEST(CustomFieldValidatorTest, InjectionPayloadNullByteInString) {
      const auto def = make_def("note", FieldDataType::String);
      const auto fields = nlohmann::json{{"note", std::string("before\0after", 12)}};
      const auto errors = validate_custom_fields(std::span{&def, 1}, fields);
      EXPECT_TRUE(errors.empty());
    }

    TEST(CustomFieldValidatorTest, InjectionPayloadCsvFormula) {
      const auto def = make_def("note", FieldDataType::String);
      const auto fields = nlohmann::json{{"note", "=CMD|calc.exe!A0"}};
      const auto errors = validate_custom_fields(std::span{&def, 1}, fields);
      EXPECT_TRUE(errors.empty());
    }

    TEST(CustomFieldValidatorTest, DeeplyNestedJsonObjectInField) {
      auto nested = nlohmann::json::object();
      auto* current = &nested;
      for (int i = 0; i < 20; ++i) {
        (*current)["level_" + std::to_string(i)] = nlohmann::json::object();
        current = &(*current)["level_" + std::to_string(i)];
      }
      (*current)["leaf"] = "deep";
      const auto def = make_def("data", FieldDataType::String);
      const auto fields = nlohmann::json{{"data", nested}};
      const auto errors = validate_custom_fields(std::span{&def, 1}, fields);
      ASSERT_EQ(errors.size(), 1U);
    }

    TEST(CustomFieldValidatorTest, IntFieldGivenFloatingPointProducesError) {
      const auto def = make_def("count", FieldDataType::Int);
      const auto fields = nlohmann::json{{"count", 3.14}};
      const auto errors = validate_custom_fields(std::span{&def, 1}, fields);
      ASSERT_EQ(errors.size(), 1U);
      EXPECT_EQ(errors.front().key, "count");
    }

    TEST(CustomFieldValidatorTest, ContradictoryConstraintsMinGreaterThanMax) {
      const auto def = make_def("age", FieldDataType::Int, false, R"({"min":100,"max":0})");
      const auto fields = nlohmann::json{{"age", 50}};
      const auto errors = validate_custom_fields(std::span{&def, 1}, fields);
      EXPECT_LE(errors.size(), 2U); // both min and max independently fail
    }

    TEST(CustomFieldValidatorTest, MaxLengthZeroEnforcesEmptyOnly) {
      const auto def = make_def("code", FieldDataType::String, false, R"({"max_length":0})");
      EXPECT_TRUE(validate_custom_fields(std::span{&def, 1}, nlohmann::json{{"code", ""}}).empty());
      EXPECT_FALSE(
          validate_custom_fields(std::span{&def, 1}, nlohmann::json{{"code", "x"}}).empty());
    }

    TEST(CustomFieldValidatorTest, FloatFieldWithExtremeBounds) {
      const auto def =
          make_def("ratio", FieldDataType::Float, false, R"({"min":-1e308,"max":1e308})");
      EXPECT_TRUE(
          validate_custom_fields(std::span{&def, 1}, nlohmann::json{{"ratio", 1e307}}).empty());
    }

    TEST(CustomFieldValidatorTest, RejectsTooManySubmittedFields) {
      // A submitted object with more than the per-entity cap is rejected outright,
      // independent of definitions, guarding against field-count DoS (review F-9).
      nlohmann::json fields = nlohmann::json::object();
      for (std::size_t i = 0; i <= k_max_custom_fields_per_entity; ++i) {
        fields["k" + std::to_string(i)] = "v";
      }
      const auto errors = validate_custom_fields(std::span<const CustomFieldDefinition>{}, fields);
      ASSERT_EQ(errors.size(), 1U);
      EXPECT_NE(errors.front().message.find("too many custom fields"), std::string::npos);
    }

  } // namespace
} // namespace fmgr::core
