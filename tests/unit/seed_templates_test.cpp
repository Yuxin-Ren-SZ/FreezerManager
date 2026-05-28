// SPDX-License-Identifier: AGPL-3.0-or-later

// Validates that every seed template JSON file under data/seed/ is structurally
// correct: parseable, has the expected position count, and every position carries
// a non-empty label and at least one accepts entry.
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <fstream>
#include <string>
#include <string_view>

namespace fmgr {
  namespace {

    constexpr std::string_view k_seed_dir = FMGR_SEED_DATA_DIR;

    [[nodiscard]] nlohmann::json load_json(std::string_view relative_path) {
      const auto path = std::string(k_seed_dir) + "/" + std::string(relative_path);
      std::ifstream file(path);
      EXPECT_TRUE(file.is_open()) << "could not open: " << path;
      nlohmann::json document;
      file >> document;
      return document;
    }

    void assert_positions_valid(const nlohmann::json& box, std::size_t expected_count) {
      ASSERT_TRUE(box.contains("positions")) << "missing 'positions' key";
      const auto& positions = box.at("positions");
      ASSERT_TRUE(positions.is_array());
      EXPECT_EQ(positions.size(), expected_count);
      for (const auto& position : positions) {
        const auto label = position.at("label").get<std::string>();
        EXPECT_FALSE(label.empty()) << "empty position label";
        const auto& accepts = position.at("accepts");
        EXPECT_TRUE(accepts.is_array() && !accepts.empty()) << "empty accepts at " << label;
      }
    }

  } // namespace

  TEST(SeedTemplates, ContainerTypeSeedFileParsesAndIsNonEmpty) {
    const auto document = load_json("container_types.json");
    ASSERT_TRUE(document.is_array());
    EXPECT_GT(document.size(), 0U);
    for (const auto& entry : document) {
      EXPECT_FALSE(entry.at("name").get<std::string>().empty());
      EXPECT_FALSE(entry.at("size_class").get<std::string>().empty());
    }
  }

  TEST(SeedTemplates, NineByNineCryoboxTemplateHas81Positions) {
    const auto box = load_json("box_types/9x9_cryobox.json");
    assert_positions_valid(box, 81);
  }

  TEST(SeedTemplates, TenByTenCryoboxTemplateHas100Positions) {
    const auto box = load_json("box_types/10x10_cryobox.json");
    assert_positions_valid(box, 100);
  }

  TEST(SeedTemplates, NinetySixWellRackTemplateHas96Positions) {
    const auto box = load_json("box_types/96_well_rack.json");
    assert_positions_valid(box, 96);
  }

  TEST(SeedTemplates, MixedEppendorfTemplateHas13Positions) {
    const auto box = load_json("box_types/mixed_eppendorf.json");
    assert_positions_valid(box, 13);
  }

} // namespace fmgr
