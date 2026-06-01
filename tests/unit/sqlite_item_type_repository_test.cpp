// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/identity.h"
#include "core/item_type.h"
#include "storage/IdentityTraits.h"
#include "storage/ItemTypeTraits.h"
#include "storage/sqlite/IdentityRepositories.h"
#include "storage/sqlite/ItemTypeRepositories.h"
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
             (std::string("freezermanager-sqlite-item-type-") + unique + "-" + std::string(suffix) +
              ".db");
    }

    void remove_sqlite_files(const std::filesystem::path& path) {
      std::filesystem::remove(path);
      std::filesystem::remove(path.string() + "-wal");
      std::filesystem::remove(path.string() + "-shm");
    }

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

    class SqliteItemTypeRepositoryTest : public ::testing::Test {
    protected:
      SqliteItemTypeRepositoryTest()
          : db_path_(sqlite_test_path("item-type")),
            backend_(SqliteBackendOptions{.database_path = db_path_.string()}) {
        register_identity_repositories(backend_);
        register_item_type_repositories(backend_);
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

      [[nodiscard]] core::LabId seed_lab(std::uint64_t low_bits) {
        const auto lab = make_lab(low_bits);
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Lab>().insert(lab, mutation_context());
        txn->commit();
        return lab.id;
      }

    private:
      std::filesystem::path db_path_;
      SqliteBackend backend_;
    };

  } // namespace

  TEST_F(SqliteItemTypeRepositoryTest, ItemTypeCrudQueryAndSoftDeleteRoundTrip) {
    const auto lab_id = seed_lab(1);
    const auto item_type = make_item_type(1, lab_id, std::nullopt, "liquid");

    // Insert
    {
      auto txn = backend().begin(IsolationLevel::Serializable);
      txn->repo<core::ItemType>().insert(item_type, mutation_context());
      txn->commit();
    }

    // Find by ID
    {
      auto txn = backend().begin(IsolationLevel::Serializable);
      const auto found = txn->repo<core::ItemType>().find_by_id(item_type.id);
      txn->commit();
      ASSERT_TRUE(found.has_value());
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
      EXPECT_EQ(found.value(), item_type);
    }

    // Update name
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

    // Soft delete
    {
      auto txn = backend().begin(IsolationLevel::Serializable);
      txn->repo<core::ItemType>().soft_delete(item_type.id, mutation_context());
      txn->commit();
    }

    // Default query excludes tombstoned
    {
      auto txn = backend().begin(IsolationLevel::Serializable);
      const auto results = txn->repo<core::ItemType>().query(Query<core::ItemType>::where(
          field<core::ItemType, core::LabId>(core::ItemType::Field::LabId) == lab_id));
      txn->commit();
      EXPECT_TRUE(results.empty());
    }

    // include_tombstoned() finds it
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

  TEST_F(SqliteItemTypeRepositoryTest, ItemTypeMutationAppendsAuditEvent) {
    const auto lab_id = seed_lab(2);
    const auto item_type = make_item_type(2, lab_id, std::nullopt, "solid");

    const auto before = backend().audit_event_count_for_tests();

    {
      auto txn = backend().begin(IsolationLevel::Serializable);
      txn->repo<core::ItemType>().insert(item_type, mutation_context());
      txn->commit();
    }

    EXPECT_GT(backend().audit_event_count_for_tests(), before);
  }

  TEST_F(SqliteItemTypeRepositoryTest, ItemTypeRejectsEmptyName) {
    const auto lab_id = seed_lab(3);
    auto bad = make_item_type(3, lab_id, std::nullopt, "");
    auto txn = backend().begin(IsolationLevel::Serializable);
    EXPECT_THROW(txn->repo<core::ItemType>().insert(bad, mutation_context()), ConstraintViolation);
  }

  TEST_F(SqliteItemTypeRepositoryTest, ItemTypeRejectsCycle) {
    const auto lab_id = seed_lab(4);
    const auto type_a = make_item_type(40, lab_id, std::nullopt, "A");
    const auto type_b = make_item_type(41, lab_id, type_a.id, "B");

    // Insert A (root)
    {
      auto txn = backend().begin(IsolationLevel::Serializable);
      txn->repo<core::ItemType>().insert(type_a, mutation_context());
      txn->commit();
    }
    // Insert B (parent = A)
    {
      auto txn = backend().begin(IsolationLevel::Serializable);
      txn->repo<core::ItemType>().insert(type_b, mutation_context());
      txn->commit();
    }
    // Attempt: set A's parent to B — would form A→B→A cycle
    auto cycle_a = type_a;
    cycle_a.parent_id = type_b.id;
    {
      auto txn = backend().begin(IsolationLevel::Serializable);
      EXPECT_THROW(txn->repo<core::ItemType>().update(cycle_a, mutation_context()),
                   ConstraintViolation);
    }
  }

  TEST_F(SqliteItemTypeRepositoryTest, ItemTypeRejectsSelfParent) {
    const auto lab_id = seed_lab(5);
    auto self_parent = make_item_type(50, lab_id, std::nullopt, "Self");
    self_parent.parent_id = self_parent.id;
    auto txn = backend().begin(IsolationLevel::Serializable);
    EXPECT_THROW(txn->repo<core::ItemType>().insert(self_parent, mutation_context()),
                 ConstraintViolation);
  }

  TEST_F(SqliteItemTypeRepositoryTest, ItemTypeQueryByLabId) {
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

  TEST_F(SqliteItemTypeRepositoryTest, CustomFieldDefinitionCrudQueryAndSoftDeleteRoundTrip) {
    const auto lab_id = seed_lab(8);
    const auto cfd = make_cfd(80, lab_id, std::nullopt, "sample_notes");

    // Insert
    {
      auto txn = backend().begin(IsolationLevel::Serializable);
      txn->repo<core::CustomFieldDefinition>().insert(cfd, mutation_context());
      txn->commit();
    }

    // Find by ID
    {
      auto txn = backend().begin(IsolationLevel::Serializable);
      const auto found = txn->repo<core::CustomFieldDefinition>().find_by_id(cfd.id);
      txn->commit();
      ASSERT_TRUE(found.has_value());
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
      EXPECT_EQ(found.value(), cfd);
    }

    // Update label
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

    // Soft delete
    {
      auto txn = backend().begin(IsolationLevel::Serializable);
      txn->repo<core::CustomFieldDefinition>().soft_delete(cfd.id, mutation_context());
      txn->commit();
    }

    // Default query excludes tombstoned
    {
      auto txn = backend().begin(IsolationLevel::Serializable);
      const auto results =
          txn->repo<core::CustomFieldDefinition>().query(Query<core::CustomFieldDefinition>::where(
              field<core::CustomFieldDefinition, core::LabId>(
                  core::CustomFieldDefinition::Field::LabId) == lab_id));
      txn->commit();
      EXPECT_TRUE(results.empty());
    }

    // include_tombstoned() finds it
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

  TEST_F(SqliteItemTypeRepositoryTest, CustomFieldDefinitionMutationAppendsAuditEvent) {
    const auto lab_id = seed_lab(9);
    const auto cfd = make_cfd(90, lab_id, std::nullopt, "audit_field");

    const auto before = backend().audit_event_count_for_tests();

    {
      auto txn = backend().begin(IsolationLevel::Serializable);
      txn->repo<core::CustomFieldDefinition>().insert(cfd, mutation_context());
      txn->commit();
    }

    EXPECT_GT(backend().audit_event_count_for_tests(), before);
  }

  TEST_F(SqliteItemTypeRepositoryTest, CustomFieldDefinitionRejectsEmptyKey) {
    const auto lab_id = seed_lab(10);
    auto bad = make_cfd(100, lab_id, std::nullopt, "");
    auto txn = backend().begin(IsolationLevel::Serializable);
    EXPECT_THROW(txn->repo<core::CustomFieldDefinition>().insert(bad, mutation_context()),
                 ConstraintViolation);
  }

  TEST_F(SqliteItemTypeRepositoryTest, CustomFieldDefinitionRejectsPhiAndIndexedCombination) {
    const auto lab_id = seed_lab(11);
    auto bad = make_cfd(110, lab_id, std::nullopt, "phi_indexed");
    bad.is_phi = true;
    bad.indexed = true;
    auto txn = backend().begin(IsolationLevel::Serializable);
    EXPECT_THROW(txn->repo<core::CustomFieldDefinition>().insert(bad, mutation_context()),
                 ConstraintViolation);
  }

  TEST_F(SqliteItemTypeRepositoryTest, CustomFieldDefinitionRejectsItemTypeFromAnotherLab) {
    const auto lab_a = seed_lab(12);
    const auto lab_b = seed_lab(13);
    // Insert an ItemType in lab_b
    const auto other_type = make_item_type(130, lab_b, std::nullopt, "OtherType");
    {
      auto txn = backend().begin(IsolationLevel::Serializable);
      txn->repo<core::ItemType>().insert(other_type, mutation_context());
      txn->commit();
    }
    // Try to attach a CFD in lab_a to an ItemType in lab_b
    auto bad = make_cfd(120, lab_a, other_type.id, "cross_lab_field");
    auto txn = backend().begin(IsolationLevel::Serializable);
    EXPECT_THROW(txn->repo<core::CustomFieldDefinition>().insert(bad, mutation_context()),
                 ConstraintViolation);
  }

  TEST_F(SqliteItemTypeRepositoryTest, CustomFieldDefinitionRejectsArchivedItemType) {
    const auto lab_id = seed_lab(14);
    const auto item_type = make_item_type(140, lab_id, std::nullopt, "ArchivedType");
    // Insert then soft-delete the ItemType
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
    // Attaching a CFD to the archived ItemType should fail
    auto bad = make_cfd(141, lab_id, item_type.id, "late_field");
    auto txn = backend().begin(IsolationLevel::Serializable);
    EXPECT_THROW(txn->repo<core::CustomFieldDefinition>().insert(bad, mutation_context()),
                 ConstraintViolation);
  }

  TEST_F(SqliteItemTypeRepositoryTest, CustomFieldDefinitionUniqueKeyPerLabScopeType) {
    const auto lab_id = seed_lab(15);
    const auto cfd1 = make_cfd(150, lab_id, std::nullopt, "dup_key");
    const auto cfd2 = make_cfd(151, lab_id, std::nullopt, "dup_key"); // same key, different id
    {
      auto txn = backend().begin(IsolationLevel::Serializable);
      txn->repo<core::CustomFieldDefinition>().insert(cfd1, mutation_context());
      txn->commit();
    }
    // Second insert with same (lab, scope_kind, item_type_id=null, key) should fail at commit.
    {
      auto txn = backend().begin(IsolationLevel::Serializable);
      txn->repo<core::CustomFieldDefinition>().insert(cfd2, mutation_context());
      EXPECT_THROW(txn->commit(), UniqueViolation);
    }
  }

  TEST_F(SqliteItemTypeRepositoryTest, CustomFieldDefinitionQueryIncludeTombstoned) {
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

} // namespace fmgr::storage
