// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/box.h"
#include "core/freezer.h"
#include "core/identity.h"
#include "storage/BoxGeometryTraits.h"
#include "storage/FreezerTraits.h"
#include "storage/IdentityTraits.h"
#include "storage/sqlite/BoxGeometryRepositories.h"
#include "storage/sqlite/IdentityRepositories.h"
#include "storage/sqlite/LayoutRepositories.h"
#include "storage/sqlite/SqliteBackend.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace fmgr::storage {
  namespace {

    [[nodiscard]] core::Uuid uuid_from_low(std::uint64_t low_bits) {
      std::array<std::uint8_t, 16> bytes{};
      for (std::size_t index = 0; index < 8; ++index) {
        bytes.at(15 - index) = static_cast<std::uint8_t>((low_bits >> (index * 8U)) & 0xffU);
      }
      return core::Uuid(bytes);
    }

    template <typename StrongId> [[nodiscard]] StrongId id_from_low(std::uint64_t low_bits) {
      return StrongId(uuid_from_low(low_bits));
    }

    [[nodiscard]] std::filesystem::path sqlite_test_path(std::string_view suffix) {
      const auto unique = std::to_string(static_cast<unsigned long long>(
                              ::testing::UnitTest::GetInstance()->random_seed())) +
                          "-" + std::to_string(reinterpret_cast<std::uintptr_t>(&suffix));
      return std::filesystem::temp_directory_path() / (std::string("freezermanager-sqlite-box-") +
                                                       unique + "-" + std::string(suffix) + ".db");
    }

    void remove_sqlite_files(const std::filesystem::path& path) {
      std::filesystem::remove(path);
      std::filesystem::remove(path.string() + "-wal");
      std::filesystem::remove(path.string() + "-shm");
    }

    [[nodiscard]] MutationContext mutation_context() {
      return MutationContext{
          .actor_user_id = id_from_low<core::UserId>(980),
          .actor_session_id = "box-session",
          .request_id = "box-request",
          .reason = "box repository test",
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
          .material = "polypropylene",
          .supplier_sku = "SKU-" + std::to_string(id_low_bits),
          .created_at =
              core::Timestamp::from_unix_micros(500 + static_cast<std::int64_t>(id_low_bits)),
      };
    }

    [[nodiscard]] core::BoxType make_box_type(std::uint64_t id_low_bits, core::LabId lab_id,
                                              const std::string& size_class) {
      return core::BoxType{
          .id = id_from_low<core::BoxTypeId>(id_low_bits),
          .lab_id = lab_id,
          .name = "BoxType " + std::to_string(id_low_bits),
          .manufacturer = "Generic",
          .sku = "BT-" + std::to_string(id_low_bits),
          .positions = {core::Position{.label = "A1", .row = 0, .col = 0, .accepts = {size_class}}},
          .created_at =
              core::Timestamp::from_unix_micros(700 + static_cast<std::int64_t>(id_low_bits)),
      };
    }

    [[nodiscard]] core::Freezer make_freezer(std::uint64_t id_low_bits, core::LabId lab_id,
                                             core::StorageContainerId layout_root_id) {
      return core::Freezer{
          .id = id_from_low<core::FreezerId>(id_low_bits),
          .lab_id = lab_id,
          .name = "Freezer " + std::to_string(id_low_bits),
          .location = "Room 101",
          .model = "ULT-80",
          .temp_target_c = -80,
          .layout_root_id = layout_root_id,
          .created_at =
              core::Timestamp::from_unix_micros(200 + static_cast<std::int64_t>(id_low_bits)),
      };
    }

    [[nodiscard]] core::StorageContainer
    make_container(std::uint64_t id_low_bits, core::LabId lab_id,
                   std::optional<core::StorageContainerId> parent_id) {
      return core::StorageContainer{
          .id = id_from_low<core::StorageContainerId>(id_low_bits),
          .lab_id = lab_id,
          .parent_id = parent_id,
          .kind = core::ContainerKind::Shelf,
          .name = "Container " + std::to_string(id_low_bits),
          .ordering_index = static_cast<int>(id_low_bits),
          .created_at =
              core::Timestamp::from_unix_micros(300 + static_cast<std::int64_t>(id_low_bits)),
      };
    }

    [[nodiscard]] core::Box make_box(std::uint64_t id_low_bits, core::LabId lab_id,
                                     core::BoxTypeId box_type_id,
                                     core::StorageContainerId storage_container_id) {
      return core::Box{
          .id = id_from_low<core::BoxId>(id_low_bits),
          .lab_id = lab_id,
          .box_type_id = box_type_id,
          .storage_container_id = storage_container_id,
          .label = "Box " + std::to_string(id_low_bits),
          .created_at =
              core::Timestamp::from_unix_micros(800 + static_cast<std::int64_t>(id_low_bits)),
      };
    }

    class SqliteBoxRepositoryTest : public ::testing::Test {
    protected:
      SqliteBoxRepositoryTest()
          : db_path_(sqlite_test_path("box-repo")),
            backend_(SqliteBackendOptions{.database_path = db_path_.string()}) {
        register_identity_repositories(backend_);
        register_layout_repositories(backend_);
        register_box_geometry_repositories(backend_);
        register_box_repositories(backend_);
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

      // Seeds a lab plus a ContainerType, BoxType, and StorageContainer in that lab.
      // Returns {box_type_id, storage_container_id} so tests can create Boxes.
      struct SeedResult {
        core::LabId lab_id;
        core::BoxTypeId box_type_id;
        core::StorageContainerId storage_container_id;
      };

      [[nodiscard]] SeedResult seed_lab_with_prereqs(std::uint64_t lab_low_bits) {
        const auto lab = make_lab(lab_low_bits, "Lab " + std::to_string(lab_low_bits));
        const auto container_type =
            make_container_type(lab_low_bits + 1000, lab.id, "cryovial_2ml");
        const auto box_type = make_box_type(lab_low_bits + 2000, lab.id, "cryovial_2ml");
        const auto storage_container = make_container(lab_low_bits + 3000, lab.id, std::nullopt);
        const auto freezer = make_freezer(lab_low_bits + 4000, lab.id, storage_container.id);

        // Each entity is seeded in its own transaction so cross-entity validation
        // (e.g. BoxType referencing ContainerType.size_class) queries committed rows.
        {
          auto txn = backend_.begin(IsolationLevel::Serializable);
          txn->repo<core::Lab>().insert(lab, mutation_context());
          txn->commit();
        }
        {
          auto txn = backend_.begin(IsolationLevel::Serializable);
          txn->repo<core::ContainerType>().insert(container_type, mutation_context());
          txn->commit();
        }
        {
          auto txn = backend_.begin(IsolationLevel::Serializable);
          txn->repo<core::BoxType>().insert(box_type, mutation_context());
          txn->commit();
        }
        {
          auto txn = backend_.begin(IsolationLevel::Serializable);
          txn->repo<core::StorageContainer>().insert(storage_container, mutation_context());
          txn->repo<core::Freezer>().insert(freezer, mutation_context());
          txn->commit();
        }

        return SeedResult{.lab_id = lab.id, .box_type_id = box_type.id, .storage_container_id = storage_container.id};
      }

    private:
      std::filesystem::path db_path_;
      SqliteBackend backend_;
    };

  } // namespace

  TEST_F(SqliteBoxRepositoryTest, BoxCrudQueryAndSoftDeleteRoundTrip) {
    const auto [lab_id, box_type_id, sc_id] = seed_lab_with_prereqs(1);
    const auto box = make_box(1, lab_id, box_type_id, sc_id);

    // Insert
    {
      auto txn = backend().begin(IsolationLevel::Serializable);
      txn->repo<core::Box>().insert(box, mutation_context());
      txn->commit();
    }

    // Find by ID
    {
      auto txn = backend().begin(IsolationLevel::Serializable);
      const auto found = txn->repo<core::Box>().find_by_id(box.id);
      txn->commit();
      ASSERT_TRUE(found.has_value());
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
      EXPECT_EQ(found.value(), box);
    }

    // Update label
    auto updated = box;
    updated.label = "Renamed Box";
    {
      auto txn = backend().begin(IsolationLevel::Serializable);
      txn->repo<core::Box>().update(updated, mutation_context());
      txn->commit();
    }
    {
      auto txn = backend().begin(IsolationLevel::Serializable);
      const auto found = txn->repo<core::Box>().find_by_id(box.id);
      txn->commit();
      ASSERT_TRUE(found.has_value());
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
      EXPECT_EQ(found.value().label, "Renamed Box");
    }

    // Query returns the box
    {
      auto txn = backend().begin(IsolationLevel::Serializable);
      const auto results = txn->repo<core::Box>().query(Query<core::Box>::where(
          field<core::Box, core::LabId>(core::Box::Field::LabId) == lab_id));
      txn->commit();
      ASSERT_EQ(results.size(), 1U);
      EXPECT_EQ(results.front().label, "Renamed Box");
    }

    // Soft-delete
    {
      auto txn = backend().begin(IsolationLevel::Serializable);
      txn->repo<core::Box>().soft_delete(box.id, mutation_context());
      txn->commit();
    }

    // Default query excludes tombstoned
    {
      auto txn = backend().begin(IsolationLevel::Serializable);
      const auto results = txn->repo<core::Box>().query(Query<core::Box>::where(
          field<core::Box, core::LabId>(core::Box::Field::LabId) == lab_id));
      txn->commit();
      EXPECT_TRUE(results.empty());
    }

    // include_tombstoned() finds it
    {
      auto txn = backend().begin(IsolationLevel::Serializable);
      const auto results = txn->repo<core::Box>().query(
          Query<core::Box>::where(field<core::Box, core::LabId>(core::Box::Field::LabId) == lab_id)
              .include_tombstoned());
      txn->commit();
      ASSERT_EQ(results.size(), 1U);
      EXPECT_TRUE(results.front().archived_at.has_value());
    }
  }

  TEST_F(SqliteBoxRepositoryTest, BoxMutationAppendsAuditEvent) {
    const auto [lab_id, box_type_id, sc_id] = seed_lab_with_prereqs(2);
    const auto before = backend().audit_event_count_for_tests();

    {
      auto txn = backend().begin(IsolationLevel::Serializable);
      txn->repo<core::Box>().insert(make_box(2, lab_id, box_type_id, sc_id), mutation_context());
      txn->commit();
    }
    EXPECT_GT(backend().audit_event_count_for_tests(), before);
  }

  TEST_F(SqliteBoxRepositoryTest, BoxRejectsEmptyLabel) {
    const auto [lab_id, box_type_id, sc_id] = seed_lab_with_prereqs(3);
    auto bad_box = make_box(3, lab_id, box_type_id, sc_id);
    bad_box.label = "";

    auto txn = backend().begin(IsolationLevel::Serializable);
    EXPECT_THROW(txn->repo<core::Box>().insert(bad_box, mutation_context()), ConstraintViolation);
  }

  TEST_F(SqliteBoxRepositoryTest, BoxRejectsBoxTypeFromAnotherLab) {
    const auto [lab_id, box_type_id, sc_id] = seed_lab_with_prereqs(4);
    const auto [other_lab_id, other_bt_id, other_sc_id] = seed_lab_with_prereqs(40);

    // Use a BoxTypeId from a different lab
    auto bad_box = make_box(4, lab_id, other_bt_id, sc_id);
    auto txn = backend().begin(IsolationLevel::Serializable);
    EXPECT_THROW(txn->repo<core::Box>().insert(bad_box, mutation_context()), ConstraintViolation);
  }

  TEST_F(SqliteBoxRepositoryTest, BoxRejectsStorageContainerFromAnotherLab) {
    const auto [lab_id, box_type_id, sc_id] = seed_lab_with_prereqs(5);
    const auto [other_lab_id, other_bt_id, other_sc_id] = seed_lab_with_prereqs(50);

    // Use a StorageContainerId from a different lab
    auto bad_box = make_box(5, lab_id, box_type_id, other_sc_id);
    auto txn = backend().begin(IsolationLevel::Serializable);
    EXPECT_THROW(txn->repo<core::Box>().insert(bad_box, mutation_context()), ConstraintViolation);
  }

  TEST_F(SqliteBoxRepositoryTest, BoxRejectsArchivedBoxType) {
    const auto [lab_id, box_type_id, sc_id] = seed_lab_with_prereqs(6);

    // Archive the BoxType
    {
      auto txn = backend().begin(IsolationLevel::Serializable);
      txn->repo<core::BoxType>().soft_delete(box_type_id, mutation_context());
      txn->commit();
    }

    auto txn = backend().begin(IsolationLevel::Serializable);
    EXPECT_THROW(
        txn->repo<core::Box>().insert(make_box(6, lab_id, box_type_id, sc_id), mutation_context()),
        ConstraintViolation);
  }

  TEST_F(SqliteBoxRepositoryTest, BoxRejectsArchivedStorageContainer) {
    const auto [lab_id, box_type_id, sc_id] = seed_lab_with_prereqs(7);

    // Archive the StorageContainer
    {
      auto txn = backend().begin(IsolationLevel::Serializable);
      txn->repo<core::StorageContainer>().soft_delete(sc_id, mutation_context());
      txn->commit();
    }

    auto txn = backend().begin(IsolationLevel::Serializable);
    EXPECT_THROW(
        txn->repo<core::Box>().insert(make_box(7, lab_id, box_type_id, sc_id), mutation_context()),
        ConstraintViolation);
  }

  TEST_F(SqliteBoxRepositoryTest, BoxQueryByStorageContainerId) {
    const auto [lab_id, box_type_id, sc_id] = seed_lab_with_prereqs(8);

    // Insert two boxes in the same container
    {
      auto txn = backend().begin(IsolationLevel::Serializable);
      txn->repo<core::Box>().insert(make_box(8, lab_id, box_type_id, sc_id), mutation_context());
      txn->repo<core::Box>().insert(make_box(9, lab_id, box_type_id, sc_id), mutation_context());
      txn->commit();
    }

    auto txn = backend().begin(IsolationLevel::Serializable);
    const auto results = txn->repo<core::Box>().query(Query<core::Box>::where(
        field<core::Box, core::StorageContainerId>(core::Box::Field::StorageContainerId) == sc_id));
    txn->commit();
    EXPECT_EQ(results.size(), 2U);
  }

  TEST_F(SqliteBoxRepositoryTest, BoxDuplicateLabelInSameLabThrows) {
    const auto [lab_id, box_type_id, sc_id] = seed_lab_with_prereqs(9);
    auto b1 = make_box(10, lab_id, box_type_id, sc_id);
    b1.label = "dup";
    auto b2 = make_box(11, lab_id, box_type_id, sc_id);
    b2.label = "dup";

    auto txn = backend().begin(IsolationLevel::Serializable);
    txn->repo<core::Box>().insert(b1, mutation_context());
    txn->repo<core::Box>().insert(b2, mutation_context());
    EXPECT_THROW(txn->commit(), UniqueViolation);
  }

  TEST_F(SqliteBoxRepositoryTest, BoxQuerySortAndLimit) {
    const auto [lab_id, box_type_id, sc_id] = seed_lab_with_prereqs(10);
    auto box_b = make_box(12, lab_id, box_type_id, sc_id);
    box_b.label = "Box B";
    auto box_a = make_box(13, lab_id, box_type_id, sc_id);
    box_a.label = "Box A";

    {
      auto txn = backend().begin(IsolationLevel::Serializable);
      txn->repo<core::Box>().insert(box_b, mutation_context());
      txn->repo<core::Box>().insert(box_a, mutation_context());
      txn->commit();
    }

    auto txn = backend().begin(IsolationLevel::Serializable);
    const auto results = txn->repo<core::Box>().query(
        Query<core::Box>::where(
            field<core::Box, core::LabId>(core::Box::Field::LabId) == lab_id)
            .order_by(field<core::Box, std::string>(core::Box::Field::Label))
            .limit(1));
    txn->commit();
    ASSERT_EQ(results.size(), 1U);
    EXPECT_EQ(results.front().label, "Box A");
  }

} // namespace fmgr::storage
