// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Tests for CustomFieldResolver — resolves the flat set of CustomFieldDefinitions
// that apply to a given ItemType by merging lab globals with all ancestor
// definitions in the taxonomy, where most-derived wins on key conflict.

#include "storage/CustomFieldResolver.h"

#include "core/identity.h"
#include "core/item_type.h"
#include "storage/IdentityTraits.h"
#include "storage/ItemTypeTraits.h"
#include "storage/sqlite/IdentityRepositories.h"
#include "storage/sqlite/ItemTypeRepositories.h"
#include "storage/sqlite/SqliteBackend.h"

#include "test_helpers.h"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace fmgr::storage {
  namespace {
    using namespace fmgr::test;

    // ---- Helpers ----

    [[nodiscard]] core::Lab make_lab(core::LabId lab_id) {
      return core::Lab{
          .id = lab_id,
          .name = "Lab " + lab_id.to_string(),
          .contact = "lab@example.org",
          .created_at = core::Timestamp::from_unix_micros(100),
          .settings_json = nlohmann::json::object(),
      };
    }

    [[nodiscard]] core::ItemType make_item_type(std::uint64_t id_low, core::LabId lab_id,
                                                std::optional<core::ItemTypeId> parent_id,
                                                std::string name) {
      return core::ItemType{
          .id = id_from_low<core::ItemTypeId>(id_low),
          .lab_id = lab_id,
          .parent_id = parent_id,
          .name = std::move(name),
          .created_at = core::Timestamp::from_unix_micros(100 + static_cast<std::int64_t>(id_low)),
      };
    }

    [[nodiscard]] core::CustomFieldDefinition
    make_cfd(std::uint64_t id_low, core::LabId lab_id, std::optional<core::ItemTypeId> item_type_id,
             std::string key, core::FieldDataType data_type = core::FieldDataType::String,
             bool required = false) {
      return core::CustomFieldDefinition{
          .id = id_from_low<core::CustomFieldDefinitionId>(id_low),
          .lab_id = lab_id,
          .scope_kind = core::ScopeKind::Sample,
          .item_type_id = item_type_id,
          .key = std::move(key),
          .label = "Label " + std::to_string(id_low),
          .data_type = data_type,
          .required = required,
          .validation_json = "{}",
          .indexed = false,
          .is_phi = false,
          .created_at = core::Timestamp::from_unix_micros(200 + static_cast<std::int64_t>(id_low)),
      };
    }

    [[nodiscard]] MutationContext mutation_context() {
      return MutationContext{
          .actor_user_id = id_from_low<core::UserId>(1),
          .actor_session_id = "cfd-resolver-session",
          .request_id = "cfd-resolver-request",
          .reason = "custom field resolver test",
      };
    }

    // Fixture: in-memory SQLite with item type and CFD repos registered.
    // Uses the default migrations (all 9) so the schema matches production.
    class CustomFieldResolverTest : public ::testing::Test {
    protected:
      void SetUp() override {
        backend_ =
            std::make_unique<SqliteBackend>(SqliteBackendOptions{.database_path = ":memory:"});
        register_identity_repositories(*backend_);
        register_item_type_repositories(*backend_);
        backend_->migrate_to_latest();
      }

      void TearDown() override {
        backend_.reset();
      }

      [[nodiscard]] IStorageBackend& backend() {
        return *backend_;
      }

      // Seed helper. Labs and item types are committed first, then the CFDs in a
      // separate transaction: a CustomFieldDefinition insert validates its
      // item_type_id against the *persisted* item_types table, and repository
      // writes are staged until commit — so a CFD cannot reference an item type
      // created in the same transaction. (Production creates them independently.)
      void seed(const std::vector<core::ItemType>& item_types,
                const std::vector<core::CustomFieldDefinition>& cfds) {
        const auto ctx = mutation_context();

        // item_types and custom_field_definitions both carry a FK to labs, so
        // insert a row for every distinct lab referenced before inserting them.
        std::set<core::LabId> lab_ids;
        for (const auto& it : item_types) {
          lab_ids.insert(it.lab_id);
        }
        for (const auto& cfd : cfds) {
          lab_ids.insert(cfd.lab_id);
        }

        {
          auto txn = backend_->begin(IsolationLevel::Serializable);
          for (const auto& lab_id : lab_ids) {
            txn->repo<core::Lab>().insert(make_lab(lab_id), ctx);
          }
          for (const auto& it : item_types) {
            txn->repo<core::ItemType>().insert(it, ctx);
          }
          txn->commit();
        }

        {
          auto txn = backend_->begin(IsolationLevel::Serializable);
          for (const auto& cfd : cfds) {
            txn->repo<core::CustomFieldDefinition>().insert(cfd, ctx);
          }
          txn->commit();
        }
      }

      std::unique_ptr<SqliteBackend> backend_;
    };

    // ---- Tests ----

    TEST_F(CustomFieldResolverTest, NoDefinitionsReturnsEmpty) {
      const auto lab_id = id_from_low<core::LabId>(100);
      const auto leaf_id = id_from_low<core::ItemTypeId>(1);
      seed({make_item_type(1, lab_id, std::nullopt, "root")}, {});

      auto txn = backend_->begin(IsolationLevel::Serializable);
      const auto resolved = resolve_custom_field_defs(*txn, lab_id, leaf_id);
      EXPECT_TRUE(resolved.empty());
    }

    TEST_F(CustomFieldResolverTest, GlobalDefinitionAppliedToLeaf) {
      const auto lab_id = id_from_low<core::LabId>(100);
      const auto leaf_id = id_from_low<core::ItemTypeId>(1);
      seed({make_item_type(1, lab_id, std::nullopt, "root")},
           {make_cfd(1, lab_id, /*global*/ std::nullopt, "source")});

      auto txn = backend_->begin(IsolationLevel::Serializable);
      const auto resolved = resolve_custom_field_defs(*txn, lab_id, leaf_id);
      ASSERT_EQ(resolved.size(), 1U);
      EXPECT_EQ(resolved[0].key, "source");
    }

    TEST_F(CustomFieldResolverTest, ItemTypeSpecificDefinitionApplied) {
      const auto lab_id = id_from_low<core::LabId>(100);
      const auto leaf_id = id_from_low<core::ItemTypeId>(2);
      seed({make_item_type(1, lab_id, std::nullopt, "root"),
            make_item_type(2, lab_id, id_from_low<core::ItemTypeId>(1), "leaf")},
           {make_cfd(1, lab_id, leaf_id, "concentration", core::FieldDataType::Float)});

      auto txn = backend_->begin(IsolationLevel::Serializable);
      const auto resolved = resolve_custom_field_defs(*txn, lab_id, leaf_id);
      ASSERT_EQ(resolved.size(), 1U);
      EXPECT_EQ(resolved[0].key, "concentration");
      EXPECT_EQ(resolved[0].data_type, core::FieldDataType::Float);
    }

    TEST_F(CustomFieldResolverTest, DescendantInheritsAncestorFields) {
      const auto lab_id = id_from_low<core::LabId>(100);
      const auto root_id = id_from_low<core::ItemTypeId>(1);
      const auto leaf_id = id_from_low<core::ItemTypeId>(3);
      seed({make_item_type(1, lab_id, std::nullopt, "root"),
            make_item_type(2, lab_id, root_id, "mid"),
            make_item_type(3, lab_id, id_from_low<core::ItemTypeId>(2), "leaf")},
           {make_cfd(1, lab_id, root_id, "ancestor_required", core::FieldDataType::String, true),
            make_cfd(2, lab_id, std::nullopt, "global_field"),
            make_cfd(3, lab_id, leaf_id, "leaf_only")});

      auto txn = backend_->begin(IsolationLevel::Serializable);
      const auto resolved = resolve_custom_field_defs(*txn, lab_id, leaf_id);
      // Should have: ancestor_required (inherited), global_field (inherited), leaf_only (own)
      EXPECT_EQ(resolved.size(), 3U);

      bool found_ancestor = false;
      bool found_global = false;
      bool found_leaf = false;
      for (const auto& cfd : resolved) {
        if (cfd.key == "ancestor_required") {
          found_ancestor = true;
          EXPECT_TRUE(cfd.required);
        }
        if (cfd.key == "global_field") {
          found_global = true;
        }
        if (cfd.key == "leaf_only") {
          found_leaf = true;
        }
      }
      EXPECT_TRUE(found_ancestor);
      EXPECT_TRUE(found_global);
      EXPECT_TRUE(found_leaf);
    }

    TEST_F(CustomFieldResolverTest, MostDerivedWinsOnKeyConflict) {
      const auto lab_id = id_from_low<core::LabId>(100);
      const auto root_id = id_from_low<core::ItemTypeId>(1);
      const auto leaf_id = id_from_low<core::ItemTypeId>(3);
      seed({make_item_type(1, lab_id, std::nullopt, "root"),
            make_item_type(2, lab_id, root_id, "mid"),
            make_item_type(3, lab_id, id_from_low<core::ItemTypeId>(2), "leaf")},
           {
               // Root: "volume" is optional
               make_cfd(1, lab_id, root_id, "volume", core::FieldDataType::Float,
                        /*required=*/false),
               // Mid: overrides to required
               make_cfd(2, lab_id, id_from_low<core::ItemTypeId>(2), "volume",
                        core::FieldDataType::Float, /*required=*/true),
               // Leaf: no override
           });

      auto txn = backend_->begin(IsolationLevel::Serializable);
      const auto resolved = resolve_custom_field_defs(*txn, lab_id, leaf_id);
      ASSERT_EQ(resolved.size(), 1U);
      // The mid-level definition (most-derived for this key) should win: required=true.
      EXPECT_EQ(resolved[0].key, "volume");
      EXPECT_TRUE(resolved[0].required);
    }

    TEST_F(CustomFieldResolverTest, DefinitionsFromAnotherLabAreExcluded) {
      const auto lab_a_id = id_from_low<core::LabId>(100);
      const auto lab_b_id = id_from_low<core::LabId>(200);
      const auto leaf_id = id_from_low<core::ItemTypeId>(1);
      seed({make_item_type(1, lab_a_id, std::nullopt, "root_a"),
            make_item_type(2, lab_b_id, std::nullopt, "root_b")},
           {make_cfd(1, lab_a_id, std::nullopt, "field_a"),
            make_cfd(2, lab_b_id, std::nullopt, "field_b")});

      auto txn = backend_->begin(IsolationLevel::Serializable);
      const auto resolved = resolve_custom_field_defs(*txn, lab_a_id, leaf_id);
      ASSERT_EQ(resolved.size(), 1U);
      EXPECT_EQ(resolved[0].key, "field_a");
    }

    TEST_F(CustomFieldResolverTest, ArchivedDefinitionsAreExcluded) {
      const auto lab_id = id_from_low<core::LabId>(100);
      const auto leaf_id = id_from_low<core::ItemTypeId>(1);
      seed({make_item_type(1, lab_id, std::nullopt, "root")},
           {make_cfd(1, lab_id, std::nullopt, "active_field")});

      // Soft-delete (archive) the CFD.
      {
        auto txn = backend_->begin(IsolationLevel::Serializable);
        const auto ctx = mutation_context();
        txn->repo<core::CustomFieldDefinition>().soft_delete(
            id_from_low<core::CustomFieldDefinitionId>(1), ctx);
        txn->commit();
      }

      auto txn = backend_->begin(IsolationLevel::Serializable);
      const auto resolved = resolve_custom_field_defs(*txn, lab_id, leaf_id);
      EXPECT_TRUE(resolved.empty());
    }

    TEST_F(CustomFieldResolverTest, NonSampleScopeKindIsExcluded) {
      const auto lab_id = id_from_low<core::LabId>(100);
      const auto leaf_id = id_from_low<core::ItemTypeId>(1);
      seed({make_item_type(1, lab_id, std::nullopt, "root")}, {});

      // Insert a Box-scoped CFD manually (not through the sample scope path).
      {
        auto txn = backend_->begin(IsolationLevel::Serializable);
        const auto ctx = mutation_context();
        core::CustomFieldDefinition box_cfd{
            .id = id_from_low<core::CustomFieldDefinitionId>(99),
            .lab_id = lab_id,
            .scope_kind = core::ScopeKind::Box,
            .item_type_id = std::nullopt,
            .key = "box_field",
            .label = "Box Field",
            .data_type = core::FieldDataType::String,
            .required = false,
            .validation_json = "{}",
            .indexed = false,
            .is_phi = false,
            .created_at = core::Timestamp::from_unix_micros(1),
        };
        txn->repo<core::CustomFieldDefinition>().insert(box_cfd, ctx);
        txn->commit();
      }

      auto txn = backend_->begin(IsolationLevel::Serializable);
      const auto resolved = resolve_custom_field_defs(*txn, lab_id, leaf_id);
      EXPECT_TRUE(resolved.empty());
    }

  } // namespace
} // namespace fmgr::storage
