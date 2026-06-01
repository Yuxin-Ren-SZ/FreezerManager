// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/freezer.h"
#include "core/identity.h"
#include "storage/FreezerTraits.h"
#include "storage/IdentityTraits.h"
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
      return std::filesystem::temp_directory_path() /
             (std::string("freezermanager-sqlite-layout-") + unique + "-" + std::string(suffix) +
              ".db");
    }

    void remove_sqlite_files(const std::filesystem::path& path) {
      std::filesystem::remove(path);
      std::filesystem::remove(path.string() + "-wal");
      std::filesystem::remove(path.string() + "-shm");
    }

    [[nodiscard]] MutationContext mutation_context() {
      return MutationContext{
          .actor_user_id = id_from_low<core::UserId>(900),
          .actor_session_id = "layout-session",
          .request_id = "layout-request",
          .reason = "layout repository test",
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

    [[nodiscard]] core::StorageContainer
    make_container(std::uint64_t id_low_bits, core::LabId lab_id,
                   std::optional<core::StorageContainerId> parent_id,
                   core::ContainerKind kind = core::ContainerKind::Compartment,
                   std::string name = "Top") {
      return core::StorageContainer{
          .id = id_from_low<core::StorageContainerId>(id_low_bits),
          .lab_id = lab_id,
          .parent_id = parent_id,
          .kind = kind,
          .name = std::move(name),
          .label = "L",
          .ordering_index = 0,
          .created_at =
              core::Timestamp::from_unix_micros(500 + static_cast<std::int64_t>(id_low_bits)),
      };
    }

    [[nodiscard]] core::Freezer make_freezer(std::uint64_t id_low_bits, core::LabId lab_id,
                                             core::StorageContainerId layout_root_id,
                                             std::string name) {
      return core::Freezer{
          .id = id_from_low<core::FreezerId>(id_low_bits),
          .lab_id = lab_id,
          .name = std::move(name),
          .location = "Bay 1",
          .model = "TSX",
          .temp_target_c = -80.0,
          .layout_root_id = layout_root_id,
          .created_at =
              core::Timestamp::from_unix_micros(1000 + static_cast<std::int64_t>(id_low_bits)),
      };
    }

    class SqliteLayoutRepositoryTest : public ::testing::Test {
    protected:
      SqliteLayoutRepositoryTest()
          : db_path_(sqlite_test_path("repositories")), backend_(SqliteBackendOptions{
                                                            .database_path = db_path_.string(),
                                                        }) {
        register_identity_repositories(backend_);
        register_layout_repositories(backend_);
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

    private:
      std::filesystem::path db_path_;
      SqliteBackend backend_;
    };

    TEST_F(SqliteLayoutRepositoryTest, FreezerWithRootContainerInsertsInOneTransaction) {
      const auto lab_entity = make_lab(1, "Lab A");
      seed_lab(lab_entity);

      const auto root =
          make_container(10, lab_entity.id, std::nullopt, core::ContainerKind::Custom, "Root");
      const auto freezer = make_freezer(11, lab_entity.id, root.id, "Minus80");

      auto transaction = backend().begin(IsolationLevel::Serializable);
      transaction->repo<core::StorageContainer>().insert(root, mutation_context());
      transaction->repo<core::Freezer>().insert(freezer, mutation_context());
      transaction->commit();

      transaction = backend().begin(IsolationLevel::Serializable);
      const auto stored = transaction->repo<core::Freezer>().find_by_id(freezer.id);
      ASSERT_TRUE(stored.has_value());
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
      EXPECT_EQ(stored.value().layout_root_id, root.id);
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
      EXPECT_EQ(stored.value().name, "Minus80");
    }

    TEST_F(SqliteLayoutRepositoryTest, FreezerWithMissingLabFailsForeignKey) {
      // No lab seeded.
      const auto root = make_container(20, id_from_low<core::LabId>(404), std::nullopt);
      const auto freezer = make_freezer(21, id_from_low<core::LabId>(404), root.id, "ghost");

      auto transaction = backend().begin(IsolationLevel::Serializable);
      transaction->repo<core::StorageContainer>().insert(root, mutation_context());
      transaction->repo<core::Freezer>().insert(freezer, mutation_context());
      EXPECT_THROW(transaction->commit(), ForeignKeyViolation);
    }

    TEST_F(SqliteLayoutRepositoryTest, RecursiveContainerTreeQueryByParent) {
      const auto lab_entity = make_lab(2, "Lab B");
      seed_lab(lab_entity);

      const auto root =
          make_container(30, lab_entity.id, std::nullopt, core::ContainerKind::Compartment, "Top");
      const auto shelf =
          make_container(31, lab_entity.id, root.id, core::ContainerKind::Shelf, "S1");
      const auto rack =
          make_container(32, lab_entity.id, shelf.id, core::ContainerKind::Rack, "R1");

      auto transaction = backend().begin(IsolationLevel::Serializable);
      auto& repo = transaction->repo<core::StorageContainer>();
      repo.insert(root, mutation_context());
      repo.insert(shelf, mutation_context());
      repo.insert(rack, mutation_context());
      transaction->commit();

      transaction = backend().begin(IsolationLevel::Serializable);
      auto& read_repo = transaction->repo<core::StorageContainer>();
      const auto children_of_root = read_repo.query(Query<core::StorageContainer>::where(
          field<core::StorageContainer, core::StorageContainerId>(
              core::StorageContainer::Field::ParentId) == root.id));
      ASSERT_EQ(children_of_root.size(), 1U);
      EXPECT_EQ(children_of_root.front().id, shelf.id);

      const auto children_of_shelf = read_repo.query(Query<core::StorageContainer>::where(
          field<core::StorageContainer, core::StorageContainerId>(
              core::StorageContainer::Field::ParentId) == shelf.id));
      ASSERT_EQ(children_of_shelf.size(), 1U);
      EXPECT_EQ(children_of_shelf.front().id, rack.id);
    }

    TEST_F(SqliteLayoutRepositoryTest, StorageContainerSelfParentIsRejected) {
      const auto lab_entity = make_lab(3, "Lab C");
      seed_lab(lab_entity);

      auto self_parent = make_container(40, lab_entity.id, std::nullopt);
      self_parent.parent_id = self_parent.id;

      auto transaction = backend().begin(IsolationLevel::Serializable);
      auto& repo = transaction->repo<core::StorageContainer>();
      EXPECT_THROW(repo.insert(self_parent, mutation_context()), ConstraintViolation);
    }

    TEST_F(SqliteLayoutRepositoryTest, ReparentingToOwnDescendantRejected) {
      const auto lab_entity = make_lab(4, "Lab D");
      seed_lab(lab_entity);

      const auto root = make_container(50, lab_entity.id, std::nullopt);
      const auto child = make_container(51, lab_entity.id, root.id);
      const auto grandchild = make_container(52, lab_entity.id, child.id);

      auto transaction = backend().begin(IsolationLevel::Serializable);
      auto& repo = transaction->repo<core::StorageContainer>();
      repo.insert(root, mutation_context());
      repo.insert(child, mutation_context());
      repo.insert(grandchild, mutation_context());
      transaction->commit();

      transaction = backend().begin(IsolationLevel::Serializable);
      auto& update_repo = transaction->repo<core::StorageContainer>();
      auto cycle_attempt = root;
      cycle_attempt.parent_id = grandchild.id; // root would now descend from itself.
      EXPECT_THROW(update_repo.update(cycle_attempt, mutation_context()), ConstraintViolation);
    }

    TEST_F(SqliteLayoutRepositoryTest, FreezerLabNameUniqueAmongLiveRowsOnly) {
      const auto lab_entity = make_lab(5, "Lab E");
      seed_lab(lab_entity);

      const auto root1 = make_container(60, lab_entity.id, std::nullopt);
      const auto freezer1 = make_freezer(61, lab_entity.id, root1.id, "Walk-in");

      auto transaction = backend().begin(IsolationLevel::Serializable);
      transaction->repo<core::StorageContainer>().insert(root1, mutation_context());
      transaction->repo<core::Freezer>().insert(freezer1, mutation_context());
      transaction->commit();

      // Duplicate name while the first freezer is live -> UniqueViolation.
      const auto root2 = make_container(62, lab_entity.id, std::nullopt);
      const auto freezer2 = make_freezer(63, lab_entity.id, root2.id, "Walk-in");
      transaction = backend().begin(IsolationLevel::Serializable);
      transaction->repo<core::StorageContainer>().insert(root2, mutation_context());
      transaction->repo<core::Freezer>().insert(freezer2, mutation_context());
      EXPECT_THROW(transaction->commit(), UniqueViolation);

      // Archive the original; same name should now be insertable.
      transaction = backend().begin(IsolationLevel::Serializable);
      transaction->repo<core::Freezer>().soft_delete(freezer1.id, mutation_context());
      transaction->commit();

      const auto root3 = make_container(64, lab_entity.id, std::nullopt);
      const auto freezer3 = make_freezer(65, lab_entity.id, root3.id, "Walk-in");
      transaction = backend().begin(IsolationLevel::Serializable);
      transaction->repo<core::StorageContainer>().insert(root3, mutation_context());
      transaction->repo<core::Freezer>().insert(freezer3, mutation_context());
      transaction->commit();

      transaction = backend().begin(IsolationLevel::Serializable);
      const auto live = transaction->repo<core::Freezer>().query(Query<core::Freezer>::all());
      ASSERT_EQ(live.size(), 1U);
      EXPECT_EQ(live.front().id, freezer3.id);
      const auto all = transaction->repo<core::Freezer>().query(
          Query<core::Freezer>::all().include_tombstoned());
      EXPECT_EQ(all.size(), 2U);
    }

    TEST_F(SqliteLayoutRepositoryTest, SoftDeleteHidesContainerFromDefaultQuery) {
      const auto lab_entity = make_lab(6, "Lab F");
      seed_lab(lab_entity);

      const auto root = make_container(70, lab_entity.id, std::nullopt);
      auto transaction = backend().begin(IsolationLevel::Serializable);
      transaction->repo<core::StorageContainer>().insert(root, mutation_context());
      transaction->commit();

      transaction = backend().begin(IsolationLevel::Serializable);
      transaction->repo<core::StorageContainer>().soft_delete(root.id, mutation_context());
      transaction->commit();

      transaction = backend().begin(IsolationLevel::Serializable);
      auto& repo = transaction->repo<core::StorageContainer>();
      EXPECT_TRUE(repo.query(Query<core::StorageContainer>::all()).empty());
      const auto archived = repo.query(Query<core::StorageContainer>::all().include_tombstoned());
      ASSERT_EQ(archived.size(), 1U);
      EXPECT_TRUE(archived.front().archived_at.has_value());
    }

    TEST_F(SqliteLayoutRepositoryTest, ContainerMutationAppendsAuditEvent) {
      const auto lab_entity = make_lab(7, "Lab G");
      seed_lab(lab_entity);
      const auto baseline_count = backend().audit_event_count_for_tests();

      const auto root = make_container(80, lab_entity.id, std::nullopt);
      auto transaction = backend().begin(IsolationLevel::Serializable);
      transaction->repo<core::StorageContainer>().insert(root, mutation_context());
      transaction->commit();

      EXPECT_EQ(backend().audit_event_count_for_tests(), baseline_count + 1U);
    }

    TEST_F(SqliteLayoutRepositoryTest, StorageContainerChildrenStillVisibleAfterParentSoftDelete) {
      const auto lab_entity = make_lab(8, "Lab H");
      seed_lab(lab_entity);

      const auto parent = make_container(90, lab_entity.id, std::nullopt);
      const auto child = make_container(91, lab_entity.id, parent.id);

      {
        auto transaction = backend().begin(IsolationLevel::Serializable);
        transaction->repo<core::StorageContainer>().insert(parent, mutation_context());
        transaction->repo<core::StorageContainer>().insert(child, mutation_context());
        transaction->commit();
      }

      {
        auto transaction = backend().begin(IsolationLevel::Serializable);
        transaction->repo<core::StorageContainer>().soft_delete(parent.id, mutation_context());
        transaction->commit();
      }

      {
        auto transaction = backend().begin(IsolationLevel::Serializable);
        const auto children = transaction->repo<core::StorageContainer>().query(
            Query<core::StorageContainer>::where(
                field<core::StorageContainer, core::StorageContainerId>(
                    core::StorageContainer::Field::ParentId) == parent.id));
        ASSERT_EQ(children.size(), 1U);
        EXPECT_EQ(children.front().id, child.id);
      }

      {
        auto transaction = backend().begin(IsolationLevel::Serializable);
        const auto found = transaction->repo<core::StorageContainer>().find_by_id(child.id);
        ASSERT_TRUE(found.has_value());
        EXPECT_EQ(found->id, child.id);
      }
    }

  } // namespace
} // namespace fmgr::storage
