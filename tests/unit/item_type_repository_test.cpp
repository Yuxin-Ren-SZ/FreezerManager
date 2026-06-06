// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/identity.h"
#include "core/item_type.h"
#include "storage/IdentityTraits.h"
#include "storage/ItemTypeTraits.h"
#include "storage/postgres/IdentityRepositories.h"
#include "storage/postgres/ItemTypeRepositories.h"
#include "storage/sqlite/IdentityRepositories.h"
#include "storage/sqlite/ItemTypeRepositories.h"

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
          .actor_user_id = id_from_low<core::UserId>(980),
          .actor_session_id = "item-type-session",
          .request_id = "item-type-request",
          .reason = "item type repository test",
      };
    }

    [[nodiscard]] core::Lab make_lab(std::uint64_t id_low_bits) {
      return core::Lab{
          .id = id_from_low<core::LabId>(id_low_bits),
          .name = "Lab " + std::to_string(id_low_bits),
          .contact = "lab@example.org",
          .created_at =
              core::Timestamp::from_unix_micros(100 + static_cast<std::int64_t>(id_low_bits)),
          .settings_json = nlohmann::json::object(),
      };
    }

    [[nodiscard]] core::ItemType make_item_type(std::uint64_t id_low_bits, core::LabId lab_id,
                                                std::optional<core::ItemTypeId> parent_id,
                                                std::string name) {
      return core::ItemType{
          .id = id_from_low<core::ItemTypeId>(id_low_bits),
          .lab_id = lab_id,
          .parent_id = parent_id,
          .name = std::move(name),
          .created_at =
              core::Timestamp::from_unix_micros(200 + static_cast<std::int64_t>(id_low_bits)),
      };
    }

    [[nodiscard]] core::CustomFieldDefinition
    make_cfd(std::uint64_t id_low_bits, core::LabId lab_id,
             std::optional<core::ItemTypeId> item_type_id, std::string key,
             core::FieldDataType data_type = core::FieldDataType::String) {
      return core::CustomFieldDefinition{
          .id = id_from_low<core::CustomFieldDefinitionId>(id_low_bits),
          .lab_id = lab_id,
          .scope_kind = core::ScopeKind::Sample,
          .item_type_id = item_type_id,
          .key = std::move(key),
          .label = "Field " + std::to_string(id_low_bits),
          .data_type = data_type,
          .required = false,
          .validation_json = "{}",
          .indexed = false,
          .is_phi = false,
          .created_at =
              core::Timestamp::from_unix_micros(300 + static_cast<std::int64_t>(id_low_bits)),
      };
    }

    class ItemTypeRepositoryTest : public ::testing::TestWithParam<BackendKind> {
    protected:
      void SetUp() override {
        if (GetParam() == BackendKind::Postgres && !postgres_test_url().has_value()) {
          GTEST_SKIP() << "FMGR_TEST_POSTGRES_URL not set; skipping Postgres repository tests";
        }
        harness_ = std::make_unique<RepoBackendHarness>(
            GetParam(),
            [](SqliteBackend& backend) {
              register_identity_repositories(backend);
              register_item_type_repositories(backend);
            },
            [](PostgresBackend& backend) {
              register_identity_repositories(backend);
              register_item_type_repositories(backend);
            });
      }

      [[nodiscard]] IStorageBackend& backend() {
        return harness_->backend();
      }

      [[nodiscard]] std::size_t audit_event_count() {
        return harness_->audit_event_count();
      }

      [[nodiscard]] core::LabId seed_lab(std::uint64_t low_bits) {
        const auto lab = make_lab(low_bits);
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Lab>().insert(lab, mutation_context());
        txn->commit();
        return lab.id;
      }

    private:
      std::unique_ptr<RepoBackendHarness> harness_;
    };

    TEST_P(ItemTypeRepositoryTest, ItemTypeCrudQueryAndSoftDeleteRoundTrip) {
      const auto lab_id = seed_lab(1);
      const auto item_type = make_item_type(1, lab_id, std::nullopt, "liquid");

      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::ItemType>().insert(item_type, mutation_context());
        txn->commit();
      }

      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        const auto found = txn->repo<core::ItemType>().find_by_id(item_type.id);
        txn->commit();
        ASSERT_TRUE(found.has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        EXPECT_EQ(found.value(), item_type);
      }

      auto updated = item_type;
      updated.name = "Liquid (updated)";
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::ItemType>().update(updated, mutation_context());
        txn->commit();
      }
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        const auto found = txn->repo<core::ItemType>().find_by_id(item_type.id);
        txn->commit();
        ASSERT_TRUE(found.has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        EXPECT_EQ(found.value().name, "Liquid (updated)");
      }

      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::ItemType>().soft_delete(item_type.id, mutation_context());
        txn->commit();
      }

      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        const auto results = txn->repo<core::ItemType>().query(Query<core::ItemType>::where(
            field<core::ItemType, core::LabId>(core::ItemType::Field::LabId) == lab_id));
        txn->commit();
        EXPECT_TRUE(results.empty());
      }

      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        const auto results = txn->repo<core::ItemType>().query(
            Query<core::ItemType>::where(
                field<core::ItemType, core::LabId>(core::ItemType::Field::LabId) == lab_id)
                .include_tombstoned());
        txn->commit();
        ASSERT_EQ(results.size(), 1U);
        EXPECT_TRUE(results.front().archived_at.has_value());
      }
    }

    TEST_P(ItemTypeRepositoryTest, ItemTypeMutationAppendsAuditEvent) {
      const auto lab_id = seed_lab(2);
      const auto item_type = make_item_type(2, lab_id, std::nullopt, "solid");
      const auto before = audit_event_count();

      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::ItemType>().insert(item_type, mutation_context());
        txn->commit();
      }

      EXPECT_GT(audit_event_count(), before);
    }

    TEST_P(ItemTypeRepositoryTest, ItemTypeRejectsEmptyName) {
      const auto lab_id = seed_lab(3);
      auto bad = make_item_type(3, lab_id, std::nullopt, "");
      auto txn = backend().begin(IsolationLevel::Serializable);
      EXPECT_THROW(txn->repo<core::ItemType>().insert(bad, mutation_context()),
                   ConstraintViolation);
    }

    TEST_P(ItemTypeRepositoryTest, ItemTypeRejectsCycle) {
      const auto lab_id = seed_lab(4);
      const auto type_a = make_item_type(40, lab_id, std::nullopt, "A");
      const auto type_b = make_item_type(41, lab_id, type_a.id, "B");

      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::ItemType>().insert(type_a, mutation_context());
        txn->commit();
      }
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::ItemType>().insert(type_b, mutation_context());
        txn->commit();
      }
      auto cycle_a = type_a;
      cycle_a.parent_id = type_b.id;
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        EXPECT_THROW(txn->repo<core::ItemType>().update(cycle_a, mutation_context()),
                     ConstraintViolation);
      }
    }

    TEST_P(ItemTypeRepositoryTest, ItemTypeRejectsSelfParent) {
      const auto lab_id = seed_lab(5);
      auto self_parent = make_item_type(50, lab_id, std::nullopt, "Self");
      self_parent.parent_id = self_parent.id;
      auto txn = backend().begin(IsolationLevel::Serializable);
      EXPECT_THROW(txn->repo<core::ItemType>().insert(self_parent, mutation_context()),
                   ConstraintViolation);
    }

    TEST_P(ItemTypeRepositoryTest, ItemTypeQueryByLabId) {
      const auto lab_a = seed_lab(6);
      const auto lab_b = seed_lab(7);
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::ItemType>().insert(make_item_type(60, lab_a, std::nullopt, "Type1"),
                                           mutation_context());
        txn->repo<core::ItemType>().insert(make_item_type(61, lab_a, std::nullopt, "Type2"),
                                           mutation_context());
        txn->repo<core::ItemType>().insert(make_item_type(62, lab_b, std::nullopt, "Other"),
                                           mutation_context());
        txn->commit();
      }
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        const auto results = txn->repo<core::ItemType>().query(Query<core::ItemType>::where(
            field<core::ItemType, core::LabId>(core::ItemType::Field::LabId) == lab_a));
        txn->commit();
        EXPECT_EQ(results.size(), 2U);
      }
    }

    TEST_P(ItemTypeRepositoryTest, CustomFieldDefinitionCrudQueryAndSoftDeleteRoundTrip) {
      const auto lab_id = seed_lab(8);
      const auto cfd = make_cfd(80, lab_id, std::nullopt, "sample_notes");

      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::CustomFieldDefinition>().insert(cfd, mutation_context());
        txn->commit();
      }

      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        const auto found = txn->repo<core::CustomFieldDefinition>().find_by_id(cfd.id);
        txn->commit();
        ASSERT_TRUE(found.has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        EXPECT_EQ(found.value(), cfd);
      }

      auto updated = cfd;
      updated.label = "Sample Notes (updated)";
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::CustomFieldDefinition>().update(updated, mutation_context());
        txn->commit();
      }
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        const auto found = txn->repo<core::CustomFieldDefinition>().find_by_id(cfd.id);
        txn->commit();
        ASSERT_TRUE(found.has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        EXPECT_EQ(found.value().label, "Sample Notes (updated)");
      }

      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::CustomFieldDefinition>().soft_delete(cfd.id, mutation_context());
        txn->commit();
      }

      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        const auto results = txn->repo<core::CustomFieldDefinition>().query(
            Query<core::CustomFieldDefinition>::where(
                field<core::CustomFieldDefinition, core::LabId>(
                    core::CustomFieldDefinition::Field::LabId) == lab_id));
        txn->commit();
        EXPECT_TRUE(results.empty());
      }

      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        const auto results = txn->repo<core::CustomFieldDefinition>().query(
            Query<core::CustomFieldDefinition>::where(
                field<core::CustomFieldDefinition, core::LabId>(
                    core::CustomFieldDefinition::Field::LabId) == lab_id)
                .include_tombstoned());
        txn->commit();
        ASSERT_EQ(results.size(), 1U);
        EXPECT_TRUE(results.front().archived_at.has_value());
      }
    }

    TEST_P(ItemTypeRepositoryTest, CustomFieldDefinitionMutationAppendsAuditEvent) {
      const auto lab_id = seed_lab(9);
      const auto cfd = make_cfd(90, lab_id, std::nullopt, "audit_field");
      const auto before = audit_event_count();

      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::CustomFieldDefinition>().insert(cfd, mutation_context());
        txn->commit();
      }

      EXPECT_GT(audit_event_count(), before);
    }

    TEST_P(ItemTypeRepositoryTest, CustomFieldDefinitionRejectsEmptyKey) {
      const auto lab_id = seed_lab(10);
      auto bad = make_cfd(100, lab_id, std::nullopt, "");
      auto txn = backend().begin(IsolationLevel::Serializable);
      EXPECT_THROW(txn->repo<core::CustomFieldDefinition>().insert(bad, mutation_context()),
                   ConstraintViolation);
    }

    TEST_P(ItemTypeRepositoryTest, CustomFieldDefinitionRejectsPhiAndIndexedCombination) {
      const auto lab_id = seed_lab(11);
      auto bad = make_cfd(110, lab_id, std::nullopt, "phi_indexed");
      bad.is_phi = true;
      bad.indexed = true;
      auto txn = backend().begin(IsolationLevel::Serializable);
      EXPECT_THROW(txn->repo<core::CustomFieldDefinition>().insert(bad, mutation_context()),
                   ConstraintViolation);
    }

    TEST_P(ItemTypeRepositoryTest, CustomFieldDefinitionRejectsItemTypeFromAnotherLab) {
      const auto lab_a = seed_lab(12);
      const auto lab_b = seed_lab(13);
      const auto other_type = make_item_type(130, lab_b, std::nullopt, "OtherType");
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::ItemType>().insert(other_type, mutation_context());
        txn->commit();
      }
      auto bad = make_cfd(120, lab_a, other_type.id, "cross_lab_field");
      auto txn = backend().begin(IsolationLevel::Serializable);
      EXPECT_THROW(txn->repo<core::CustomFieldDefinition>().insert(bad, mutation_context()),
                   ConstraintViolation);
    }

    TEST_P(ItemTypeRepositoryTest, CustomFieldDefinitionRejectsArchivedItemType) {
      const auto lab_id = seed_lab(14);
      const auto item_type = make_item_type(140, lab_id, std::nullopt, "ArchivedType");
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::ItemType>().insert(item_type, mutation_context());
        txn->commit();
      }
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::ItemType>().soft_delete(item_type.id, mutation_context());
        txn->commit();
      }
      auto bad = make_cfd(141, lab_id, item_type.id, "late_field");
      auto txn = backend().begin(IsolationLevel::Serializable);
      EXPECT_THROW(txn->repo<core::CustomFieldDefinition>().insert(bad, mutation_context()),
                   ConstraintViolation);
    }

    TEST_P(ItemTypeRepositoryTest, CustomFieldDefinitionUniqueKeyPerLabScopeType) {
      const auto lab_id = seed_lab(15);
      const auto cfd1 = make_cfd(150, lab_id, std::nullopt, "dup_key");
      const auto cfd2 = make_cfd(151, lab_id, std::nullopt, "dup_key"); // same key, different id
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::CustomFieldDefinition>().insert(cfd1, mutation_context());
        txn->commit();
      }
      // Same (lab, scope_kind, item_type_id=null, key) collides on the unique
      // index. SQLite fails at commit; Postgres fails on the immediate insert.
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        EXPECT_THROW(
            {
              txn->repo<core::CustomFieldDefinition>().insert(cfd2, mutation_context());
              txn->commit();
            },
            UniqueViolation);
      }
    }

    TEST_P(ItemTypeRepositoryTest, CustomFieldDefinitionQueryIncludeTombstoned) {
      const auto lab_id = seed_lab(16);
      const auto cfd = make_cfd(160, lab_id, std::nullopt, "test_field");

      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::CustomFieldDefinition>().insert(cfd, mutation_context());
        txn->commit();
      }

      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::CustomFieldDefinition>().soft_delete(cfd.id, mutation_context());
        txn->commit();
      }

      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        const auto results = txn->repo<core::CustomFieldDefinition>().query(
            Query<core::CustomFieldDefinition>::all());
        txn->commit();
        EXPECT_TRUE(results.empty());
      }

      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        const auto results = txn->repo<core::CustomFieldDefinition>().query(
            Query<core::CustomFieldDefinition>::all().include_tombstoned());
        txn->commit();
        ASSERT_EQ(results.size(), 1U);
        EXPECT_TRUE(results.front().archived_at.has_value());
      }
    }

    INSTANTIATE_TEST_SUITE_P(Backends, ItemTypeRepositoryTest,
                             ::testing::Values(BackendKind::Sqlite, BackendKind::Postgres),
                             [](const ::testing::TestParamInfo<BackendKind>& info) {
                               return backend_kind_name(info.param);
                             });

  } // namespace
} // namespace fmgr::storage
