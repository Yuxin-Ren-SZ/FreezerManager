// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Tests for the tighten-not-loosen rule (N5): what a more-derived
// CustomFieldDefinition may and may not change about the less-derived one of
// the same key that it shadows. The SPA's field form has the same table in
// `src/web/src/features/item-types/itemTypeModel.test.ts`; the two must agree,
// because the browser is now the fast-feedback half rather than the only half.

#include "core/custom_field_tightening.h"

#include "test_helpers.h"
#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace fmgr::core {
  namespace {
    using namespace fmgr::test;

    [[nodiscard]] CustomFieldDefinition make_cfd(const std::string& key, FieldDataType data_type,
                                                 bool required = false,
                                                 const std::string& validation_json = "{}",
                                                 bool is_phi = false) {
      return CustomFieldDefinition{
          .id = id_from_low<CustomFieldDefinitionId>(1),
          .lab_id = id_from_low<LabId>(2),
          .scope_kind = ScopeKind::Sample,
          .item_type_id = std::nullopt,
          .key = key,
          .label = "Test Field",
          .data_type = data_type,
          .required = required,
          .validation_json = validation_json,
          .indexed = false,
          .is_phi = is_phi,
          .created_at = Timestamp::from_unix_micros(1000),
      };
    }

    // Both parameters are definitions of the same key, so the type system cannot
    // tell a swapped call from a correct one; the parameter names carry it.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    [[nodiscard]] std::vector<std::string> constraints_of(const CustomFieldDefinition& parent,
                                                          const CustomFieldDefinition& child) {
      std::vector<std::string> constraints;
      for (const auto& violation : tighten_violations(parent, child)) {
        constraints.push_back(violation.constraint);
      }
      return constraints;
    }

    TEST(CustomFieldTighteningTest, IdenticalDefinitionIsAllowed) {
      const auto parent =
          make_cfd("notes", FieldDataType::String, /*required=*/true, R"({"max_length":20})");
      EXPECT_TRUE(tighten_violations(parent, parent).empty());
    }

    // The rule's name, tested on its own rather than as a case of "narrower":
    // dropping `required` is not a constraint change, it is the requirement
    // disappearing for the whole subtree.
    TEST(CustomFieldTighteningTest, DroppingRequiredFieldIsRefused) {
      const auto parent = make_cfd("patient_id", FieldDataType::String, /*required=*/true);
      const auto child = make_cfd("patient_id", FieldDataType::String, /*required=*/false);
      const auto violations = tighten_violations(parent, child);
      ASSERT_EQ(violations.size(), 1U);
      EXPECT_EQ(violations.front().constraint, "required");
      EXPECT_NE(violations.front().message.find("required"), std::string::npos);
    }

    TEST(CustomFieldTighteningTest, MakingAnOptionalFieldRequiredIsAllowed) {
      const auto parent = make_cfd("notes", FieldDataType::String, /*required=*/false);
      const auto child = make_cfd("notes", FieldDataType::String, /*required=*/true);
      EXPECT_TRUE(tighten_violations(parent, child).empty());
    }

    TEST(CustomFieldTighteningTest, KeepingRequiredIsAllowed) {
      const auto parent = make_cfd("notes", FieldDataType::String, /*required=*/true);
      const auto child = make_cfd("notes", FieldDataType::String, /*required=*/true);
      EXPECT_TRUE(tighten_violations(parent, child).empty());
    }

    TEST(CustomFieldTighteningTest, NarrowingMaxLengthIsAllowedAndRaisingOrDroppingItIsNot) {
      const auto parent = make_cfd("notes", FieldDataType::String, false, R"({"max_length":20})");

      EXPECT_TRUE(tighten_violations(parent, make_cfd("notes", FieldDataType::String, false,
                                                      R"({"max_length":5})"))
                      .empty());
      // Equal is a tightening, not a widening.
      EXPECT_TRUE(tighten_violations(parent, make_cfd("notes", FieldDataType::String, false,
                                                      R"({"max_length":20})"))
                      .empty());
      EXPECT_EQ(constraints_of(parent, make_cfd("notes", FieldDataType::String, false,
                                                R"({"max_length":40})")),
                std::vector<std::string>{"max_length"});
      EXPECT_EQ(constraints_of(parent, make_cfd("notes", FieldDataType::String, false, "{}")),
                std::vector<std::string>{"max_length"});
    }

    TEST(CustomFieldTighteningTest, NarrowingANumericRangeIsAllowedAndWideningEitherEndIsNot) {
      const auto parent =
          make_cfd("aliquot_count", FieldDataType::Int, false, R"({"min":1,"max":10})");

      EXPECT_TRUE(tighten_violations(parent, make_cfd("aliquot_count", FieldDataType::Int, false,
                                                      R"({"min":2,"max":4})"))
                      .empty());
      EXPECT_EQ(constraints_of(parent, make_cfd("aliquot_count", FieldDataType::Int, false,
                                                R"({"min":0,"max":10})")),
                std::vector<std::string>{"min"});
      EXPECT_EQ(constraints_of(parent, make_cfd("aliquot_count", FieldDataType::Int, false,
                                                R"({"min":1,"max":99})")),
                std::vector<std::string>{"max"});
      EXPECT_EQ(constraints_of(
                    parent, make_cfd("aliquot_count", FieldDataType::Int, false, R"({"min":1})")),
                std::vector<std::string>{"max"});
      // Both ends dropped is reported once per end, so the message names what
      // actually disappeared.
      EXPECT_EQ(constraints_of(parent, make_cfd("aliquot_count", FieldDataType::Int, false, "{}")),
                (std::vector<std::string>{"min", "max"}));
    }

    TEST(CustomFieldTighteningTest, DateBoundsCompareLexicographically) {
      const auto parent = make_cfd("collected_on", FieldDataType::Date, false,
                                   R"({"min":"2020-01-01","max":"2020-12-31"})");

      EXPECT_TRUE(tighten_violations(parent, make_cfd("collected_on", FieldDataType::Date, false,
                                                      R"({"min":"2020-06-01","max":"2020-07-01"})"))
                      .empty());
      EXPECT_EQ(constraints_of(parent, make_cfd("collected_on", FieldDataType::Date, false,
                                                R"({"min":"2019-01-01","max":"2020-12-31"})")),
                std::vector<std::string>{"min"});
    }

    TEST(CustomFieldTighteningTest, EnumSubsetIsAllowedAndAnAddedValueIsNot) {
      const auto parent = make_cfd("tube_type", FieldDataType::Enum, false,
                                   R"({"values":["EDTA","heparin","plain"]})");

      EXPECT_TRUE(tighten_violations(parent, make_cfd("tube_type", FieldDataType::Enum, false,
                                                      R"({"values":["EDTA","heparin"]})"))
                      .empty());
      EXPECT_EQ(constraints_of(parent, make_cfd("tube_type", FieldDataType::Enum, false,
                                                R"({"values":["EDTA","citrate"]})")),
                std::vector<std::string>{"values"});
      EXPECT_EQ(constraints_of(parent, make_cfd("tube_type", FieldDataType::Enum, false, "{}")),
                std::vector<std::string>{"values"});
    }

    TEST(CustomFieldTighteningTest, ChangingDataTypeIsRefused) {
      const auto parent = make_cfd("value", FieldDataType::String);
      EXPECT_EQ(constraints_of(parent, make_cfd("value", FieldDataType::Int)),
                std::vector<std::string>{"data_type"});
    }

    TEST(CustomFieldTighteningTest, ChangingScopeIsRefused) {
      auto parent = make_cfd("value", FieldDataType::String);
      auto child = make_cfd("value", FieldDataType::String);
      child.scope_kind = ScopeKind::Box;
      EXPECT_EQ(constraints_of(parent, child), std::vector<std::string>{"scope_kind"});
    }

    TEST(CustomFieldTighteningTest, DroppingPhiIsRefused) {
      const auto parent = make_cfd("ssn", FieldDataType::String, false, "{}", /*is_phi=*/true);
      const auto child = make_cfd("ssn", FieldDataType::String, false, "{}", /*is_phi=*/false);
      EXPECT_EQ(constraints_of(parent, child), std::vector<std::string>{"is_phi"});
    }

    TEST(CustomFieldTighteningTest, KeepingPhiIsAllowed) {
      const auto parent = make_cfd("ssn", FieldDataType::String, false, "{}", /*is_phi=*/true);
      EXPECT_TRUE(tighten_violations(parent, parent).empty());
    }

    // Deliberate, and the reason L10 stays satisfiable: `indexed` is a lookup
    // structure, not a rule about which values are valid, so removing it does
    // not widen what the child accepts.
    TEST(CustomFieldTighteningTest, RemovingAnIndexIsNotAViolation) {
      auto parent = make_cfd("notes", FieldDataType::String);
      parent.indexed = true;
      const auto child = make_cfd("notes", FieldDataType::String);
      EXPECT_TRUE(tighten_violations(parent, child).empty());
    }

    TEST(CustomFieldTighteningTest, UnknownAndMalformedConstraintsAreIgnored) {
      const auto parent = make_cfd("notes", FieldDataType::String, false, "not json");
      // A constraint the validator does not implement is not one to tighten.
      const auto child = make_cfd("notes", FieldDataType::String, false, R"({"regex":"^a+$"})");
      EXPECT_TRUE(tighten_violations(parent, child).empty());
      // A `values` array that is not all strings is ignored by the validator, so
      // it is not treated as an inherited set either.
      const auto mixed = make_cfd("notes", FieldDataType::String, false, R"({"values":["a",1]})");
      EXPECT_TRUE(
          tighten_violations(mixed, make_cfd("notes", FieldDataType::String, false, "{}")).empty());
    }

  } // namespace
} // namespace fmgr::core
