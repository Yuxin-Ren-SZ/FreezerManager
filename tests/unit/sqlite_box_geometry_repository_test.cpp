// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/box.h"
#include "core/identity.h"
#include "storage/BoxGeometryTraits.h"
#include "storage/IdentityTraits.h"
#include "storage/sqlite/BoxGeometryRepositories.h"
#include "storage/sqlite/IdentityRepositories.h"
#include "storage/sqlite/SqliteBackend.h"

#include "test_helpers.h"
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fmgr::storage {
  namespace {
    using namespace fmgr::test;



    [[nodiscard]] std::filesystem::path sqlite_test_path(std::string_view suffix) {
      const auto unique = std::to_string(static_cast<unsigned long long>(
                              ::testing::UnitTest::GetInstance()->random_seed())) +
                          "-" + std::to_string(reinterpret_cast<std::uintptr_t>(&suffix));
      return std::filesystem::temp_directory_path() /
             (std::string("freezermanager-sqlite-box-geometry-") + unique + "-" +
              std::string(suffix) + ".db");
    }


    [[nodiscard]] MutationContext mutation_context() {
      return MutationContext{
          .actor_user_id = id_from_low<core::UserId>(990),
          .actor_session_id = "box-geometry-session",
          .request_id = "box-geometry-request",
          .reason = "box geometry repository test",
      };
    }

    [[nodiscard]] core::Lab make_lab(std::uint64_t id_low_bits, std::string name) {
      return core::Lab{
          .id = id_from_low<core::LabId>(id_low_bits),
          .name = std::move(name),
          .contact = "lab@example.org",
          .created_at =
              core::Timestamp::from_unix_micros(100 + static_cast<std::int64_t>(id_low_bits)),
          .settings_json = nlohmann::json::object(),
      };
    }

    [[nodiscard]] core::ContainerType
    make_container_type(std::uint64_t id_low_bits, core::LabId lab_id, std::string size_class) {
      return core::ContainerType{
          .id = id_from_low<core::ContainerTypeId>(id_low_bits),
          .lab_id = lab_id,
          .name = "Container " + std::to_string(id_low_bits),
          .size_class = std::move(size_class),
          .outer_dimensions_mm =
              core::OuterDimensionsMm{.width = 12.5, .height = 40.0, .depth = 12.5},
          .material = "polypropylene",
          .supplier_sku = "SKU-" + std::to_string(id_low_bits),
          .created_at =
              core::Timestamp::from_unix_micros(500 + static_cast<std::int64_t>(id_low_bits)),
      };
    }

    [[nodiscard]] core::BoxType make_box_type(std::uint64_t id_low_bits, core::LabId lab_id,
                                              std::vector<core::Position> positions) {
      return core::BoxType{
          .id = id_from_low<core::BoxTypeId>(id_low_bits),
          .lab_id = lab_id,
          .name = "Box " + std::to_string(id_low_bits),
          .manufacturer = "Generic",
          .sku = "BOX-" + std::to_string(id_low_bits),
          .positions = std::move(positions),
          .created_at =
              core::Timestamp::from_unix_micros(700 + static_cast<std::int64_t>(id_low_bits)),
      };
    }

    [[nodiscard]] std::vector<core::Position> two_positions() {
      return {
          core::Position{.label = "A1", .row = 0, .col = 0, .accepts = {"cryovial_2ml"}},
          core::Position{.label = "A2", .row = 0, .col = 1, .z = 1, .accepts = {"cryovial_2ml"}},
      };
    }

    class SqliteBoxGeometryRepositoryTest : public ::testing::Test {
    protected:
      SqliteBoxGeometryRepositoryTest()
          : db_path_(sqlite_test_path("repositories")), backend_(SqliteBackendOptions{
                                                            .database_path = db_path_.string(),
                                                        }) {
        register_identity_repositories(backend_);
        register_box_geometry_repositories(backend_);
      }

      void SetUp() override {
        remove_sqlite_files(db_path_);
        backend_.migrate_to_latest();
      }

      void TearDown() override {
        remove_sqlite_files(db_path_);
      }

      [[nodiscard]] SqliteBackend& backend() {
        return backend_;
      }

      void seed_lab(const core::Lab& lab_entity) {
        auto transaction = backend_.begin(IsolationLevel::Serializable);
        transaction->repo<core::Lab>().insert(lab_entity, mutation_context());
        transaction->commit();
      }

      void seed_container_type(const core::ContainerType& container_type) {
        auto transaction = backend_.begin(IsolationLevel::Serializable);
        transaction->repo<core::ContainerType>().insert(container_type, mutation_context());
        transaction->commit();
      }

    private:
      std::filesystem::path db_path_;
      SqliteBackend backend_;
    };

    TEST_F(SqliteBoxGeometryRepositoryTest, ContainerTypeCrudQueryAndSoftDeleteRoundTrip) {
      const auto lab_entity = make_lab(1, "Lab A");
      seed_lab(lab_entity);
      const auto container_type = make_container_type(10, lab_entity.id, "cryovial_2ml");

      auto transaction = backend().begin(IsolationLevel::Serializable);
      transaction->repo<core::ContainerType>().insert(container_type, mutation_context());
      transaction->commit();

      transaction = backend().begin(IsolationLevel::Serializable);
      auto& repo = transaction->repo<core::ContainerType>();
      const auto by_size_class = repo.query(Query<core::ContainerType>::where(
          field<core::ContainerType, std::string>(core::ContainerType::Field::SizeClass) ==
          std::string("cryovial_2ml")));
      ASSERT_EQ(by_size_class.size(), 1U);
      EXPECT_EQ(by_size_class.front(), container_type);

      auto updated = container_type;
      updated.name = "Updated cryovial";
      repo.update(updated, mutation_context());
      transaction->commit();

      transaction = backend().begin(IsolationLevel::Serializable);
      transaction->repo<core::ContainerType>().soft_delete(container_type.id, mutation_context());
      transaction->commit();

      transaction = backend().begin(IsolationLevel::Serializable);
      auto& read_repo = transaction->repo<core::ContainerType>();
      EXPECT_TRUE(read_repo.query(Query<core::ContainerType>::all()).empty());
      const auto archived = read_repo.query(Query<core::ContainerType>::all().include_tombstoned());
      ASSERT_EQ(archived.size(), 1U);
      EXPECT_TRUE(archived.front().archived_at.has_value());
    }

    TEST_F(SqliteBoxGeometryRepositoryTest, BoxTypeWithPositionsRoundTripsAndUpdatesPositions) {
      const auto lab_entity = make_lab(2, "Lab B");
      seed_lab(lab_entity);
      seed_container_type(make_container_type(20, lab_entity.id, "cryovial_2ml"));
      seed_container_type(make_container_type(21, lab_entity.id, "falcon_15ml"));

      auto box_type = make_box_type(30, lab_entity.id, two_positions());
      auto transaction = backend().begin(IsolationLevel::Serializable);
      transaction->repo<core::BoxType>().insert(box_type, mutation_context());
      transaction->commit();

      transaction = backend().begin(IsolationLevel::Serializable);
      auto stored = transaction->repo<core::BoxType>().find_by_id(box_type.id);
      ASSERT_TRUE(stored.has_value());
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
      EXPECT_EQ(stored.value(), box_type);

      box_type.positions = {
          core::Position{.label = "B1", .row = 1, .col = 0, .accepts = {"falcon_15ml"}},
      };
      transaction = backend().begin(IsolationLevel::Serializable);
      transaction->repo<core::BoxType>().update(box_type, mutation_context());
      transaction->commit();

      transaction = backend().begin(IsolationLevel::Serializable);
      stored = transaction->repo<core::BoxType>().find_by_id(box_type.id);
      ASSERT_TRUE(stored.has_value());
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
      EXPECT_EQ(stored->positions, box_type.positions);
    }

    TEST_F(SqliteBoxGeometryRepositoryTest, BoxTypeRejectsDuplicatePositionLabels) {
      const auto lab_entity = make_lab(3, "Lab C");
      seed_lab(lab_entity);
      seed_container_type(make_container_type(40, lab_entity.id, "cryovial_2ml"));

      auto box_type = make_box_type(41, lab_entity.id, two_positions());
      box_type.positions.at(1).label = "A1";

      auto transaction = backend().begin(IsolationLevel::Serializable);
      EXPECT_THROW(transaction->repo<core::BoxType>().insert(box_type, mutation_context()),
                   ConstraintViolation);
    }

    TEST_F(SqliteBoxGeometryRepositoryTest, BoxTypeRejectsEmptyAccepts) {
      const auto lab_entity = make_lab(4, "Lab D");
      seed_lab(lab_entity);

      auto box_type = make_box_type(50, lab_entity.id, two_positions());
      box_type.positions.front().accepts.clear();

      auto transaction = backend().begin(IsolationLevel::Serializable);
      EXPECT_THROW(transaction->repo<core::BoxType>().insert(box_type, mutation_context()),
                   ConstraintViolation);
    }

    TEST_F(SqliteBoxGeometryRepositoryTest, BoxTypeRejectsUnknownSizeClass) {
      const auto lab_entity = make_lab(5, "Lab E");
      seed_lab(lab_entity);
      seed_container_type(make_container_type(60, lab_entity.id, "cryovial_2ml"));

      auto box_type = make_box_type(61, lab_entity.id, two_positions());
      box_type.positions.front().accepts = {"missing_size"};

      auto transaction = backend().begin(IsolationLevel::Serializable);
      EXPECT_THROW(transaction->repo<core::BoxType>().insert(box_type, mutation_context()),
                   ConstraintViolation);
    }

    TEST_F(SqliteBoxGeometryRepositoryTest, BoxTypeRejectsSizeClassFromAnotherLab) {
      const auto lab_a = make_lab(6, "Lab F");
      const auto lab_b = make_lab(7, "Lab G");
      seed_lab(lab_a);
      seed_lab(lab_b);
      seed_container_type(make_container_type(70, lab_b.id, "cryovial_2ml"));

      const auto box_type = make_box_type(71, lab_a.id, two_positions());

      auto transaction = backend().begin(IsolationLevel::Serializable);
      EXPECT_THROW(transaction->repo<core::BoxType>().insert(box_type, mutation_context()),
                   ConstraintViolation);
    }

    TEST_F(SqliteBoxGeometryRepositoryTest, ContainerTypeRejectsInvalidDimensions) {
      const auto lab_entity = make_lab(8, "Lab H");
      seed_lab(lab_entity);
      auto container_type = make_container_type(80, lab_entity.id, "cryovial_2ml");
      container_type.outer_dimensions_mm =
          core::OuterDimensionsMm{.width = 0.0, .height = 40.0, .depth = 12.5};

      auto transaction = backend().begin(IsolationLevel::Serializable);
      EXPECT_THROW(
          transaction->repo<core::ContainerType>().insert(container_type, mutation_context()),
          ConstraintViolation);
    }

    TEST_F(SqliteBoxGeometryRepositoryTest, BoxTypeMutationAppendsAuditEvent) {
      const auto lab_entity = make_lab(9, "Lab I");
      seed_lab(lab_entity);
      seed_container_type(make_container_type(90, lab_entity.id, "cryovial_2ml"));
      const auto baseline_count = backend().audit_event_count_for_tests();

      const auto box_type = make_box_type(91, lab_entity.id, two_positions());
      auto transaction = backend().begin(IsolationLevel::Serializable);
      transaction->repo<core::BoxType>().insert(box_type, mutation_context());
      transaction->commit();

      EXPECT_EQ(backend().audit_event_count_for_tests(), baseline_count + 1U);
    }

  } // namespace
} // namespace fmgr::storage
