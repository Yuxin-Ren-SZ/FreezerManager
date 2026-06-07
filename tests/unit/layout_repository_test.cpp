// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/freezer.h"
#include "core/identity.h"
#include "storage/FreezerTraits.h"
#include "storage/IdentityTraits.h"
#include "storage/postgres/IdentityRepositories.h"
#include "storage/postgres/LayoutRepositories.h"
#include "storage/sqlite/IdentityRepositories.h"
#include "storage/sqlite/LayoutRepositories.h"

#include "repo_backend_harness.h"
#include "test_helpers.h"
#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace fmgr::storage {
  namespace {
    using namespace fmgr::test;

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

    class LayoutRepositoryTest : public ::testing::TestWithParam<BackendKind> {
    protected:
      void SetUp() override {
        if (GetParam() == BackendKind::Postgres && !postgres_test_url().has_value()) {
          GTEST_SKIP() << "FMGR_TEST_POSTGRES_URL not set; skipping Postgres repository tests";
        }
        harness_ = std::make_unique<RepoBackendHarness>(
            GetParam(),
            [](SqliteBackend& backend) {
              register_identity_repositories(backend);
              register_layout_repositories(backend);
            },
            [](PostgresBackend& backend) {
              register_identity_repositories(backend);
              register_layout_repositories(backend);
            });
      }

      [[nodiscard]] IStorageBackend& backend() {
        return harness_->backend();
      }

      [[nodiscard]] std::size_t audit_event_count() {
        return harness_->audit_event_count();
      }

      void seed_lab(const core::Lab& lab_entity) {
        auto transaction = backend().begin(IsolationLevel::Serializable);
        transaction->repo<core::Lab>().insert(lab_entity, mutation_context());
        transaction->commit();
      }

    private:
      std::unique_ptr<RepoBackendHarness> harness_;
    };

    TEST_P(LayoutRepositoryTest, FreezerWithRootContainerInsertsInOneTransaction) {
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

    TEST_P(LayoutRepositoryTest, FreezerWithMissingLabFailsForeignKey) {
      // No lab seeded. The lab_id foreign keys are deferred, so the violation
      // surfaces at commit on both backends.
      const auto root = make_container(20, id_from_low<core::LabId>(404), std::nullopt);
      const auto freezer = make_freezer(21, id_from_low<core::LabId>(404), root.id, "ghost");

      auto transaction = backend().begin(IsolationLevel::Serializable);
      transaction->repo<core::StorageContainer>().insert(root, mutation_context());
      transaction->repo<core::Freezer>().insert(freezer, mutation_context());
      EXPECT_THROW(transaction->commit(), ForeignKeyViolation);
    }

    TEST_P(LayoutRepositoryTest, RecursiveContainerTreeQueryByParent) {
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

    TEST_P(LayoutRepositoryTest, StorageContainerSelfParentIsRejected) {
      const auto lab_entity = make_lab(3, "Lab C");
      seed_lab(lab_entity);

      auto self_parent = make_container(40, lab_entity.id, std::nullopt);
      self_parent.parent_id = self_parent.id;

      auto transaction = backend().begin(IsolationLevel::Serializable);
      auto& repo = transaction->repo<core::StorageContainer>();
      EXPECT_THROW(repo.insert(self_parent, mutation_context()), ConstraintViolation);
    }

    TEST_P(LayoutRepositoryTest, ReparentingToOwnDescendantRejected) {
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

    TEST_P(LayoutRepositoryTest, FreezerLabNameUniqueAmongLiveRowsOnly) {
      const auto lab_entity = make_lab(5, "Lab E");
      seed_lab(lab_entity);

      const auto root1 = make_container(60, lab_entity.id, std::nullopt);
      const auto freezer1 = make_freezer(61, lab_entity.id, root1.id, "Walk-in");

      auto transaction = backend().begin(IsolationLevel::Serializable);
      transaction->repo<core::StorageContainer>().insert(root1, mutation_context());
      transaction->repo<core::Freezer>().insert(freezer1, mutation_context());
      transaction->commit();

      // Duplicate name while the first freezer is live -> UniqueViolation. SQLite
      // stages and fails the partial unique index at commit; Postgres fails on the
      // immediate insert. Wrap both so either path is caught.
      const auto root2 = make_container(62, lab_entity.id, std::nullopt);
      const auto freezer2 = make_freezer(63, lab_entity.id, root2.id, "Walk-in");
      transaction = backend().begin(IsolationLevel::Serializable);
      transaction->repo<core::StorageContainer>().insert(root2, mutation_context());
      EXPECT_THROW(
          {
            transaction->repo<core::Freezer>().insert(freezer2, mutation_context());
            transaction->commit();
          },
          UniqueViolation);

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

    TEST_P(LayoutRepositoryTest, SoftDeleteHidesContainerFromDefaultQuery) {
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

    TEST_P(LayoutRepositoryTest, ContainerMutationAppendsAuditEvent) {
      const auto lab_entity = make_lab(7, "Lab G");
      seed_lab(lab_entity);
      const auto baseline_count = audit_event_count();

      const auto root = make_container(80, lab_entity.id, std::nullopt);
      auto transaction = backend().begin(IsolationLevel::Serializable);
      transaction->repo<core::StorageContainer>().insert(root, mutation_context());
      transaction->commit();

      EXPECT_EQ(audit_event_count(), baseline_count + 1U);
    }

    TEST_P(LayoutRepositoryTest, StorageContainerChildrenStillVisibleAfterParentSoftDelete) {
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
        const auto children =
            transaction->repo<core::StorageContainer>().query(Query<core::StorageContainer>::where(
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

    INSTANTIATE_TEST_SUITE_P(Backends, LayoutRepositoryTest,
                             ::testing::Values(BackendKind::Sqlite, BackendKind::Postgres),
                             [](const ::testing::TestParamInfo<BackendKind>& info) {
                               return backend_kind_name(info.param);
                             });

  } // namespace
} // namespace fmgr::storage
