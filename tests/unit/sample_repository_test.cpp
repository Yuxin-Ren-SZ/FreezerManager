// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/box.h"
#include "core/custom_field_validator.h"
#include "core/freezer.h"
#include "core/identity.h"
#include "core/item_type.h"
#include "core/sample.h"
#include "storage/BoxGeometryTraits.h"
#include "storage/CustomFieldResolver.h"
#include "storage/FreezerTraits.h"
#include "storage/IdentityTraits.h"
#include "storage/ItemTypeTraits.h"
#include "storage/SampleOps.h"
#include "storage/SampleTraits.h"
#include "storage/postgres/BoxGeometryRepositories.h"
#include "storage/postgres/IdentityRepositories.h"
#include "storage/postgres/ItemTypeRepositories.h"
#include "storage/postgres/LayoutRepositories.h"
#include "storage/postgres/SampleRepositories.h"
#include "storage/sqlite/BoxGeometryRepositories.h"
#include "storage/sqlite/IdentityRepositories.h"
#include "storage/sqlite/ItemTypeRepositories.h"
#include "storage/sqlite/LayoutRepositories.h"
#include "storage/sqlite/SampleRepositories.h"

#include "repo_backend_harness.h"
#include "test_helpers.h"
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace fmgr::storage {
  namespace {
    using namespace fmgr::test;

    [[nodiscard]] MutationContext mutation_context() {
      return MutationContext{
          .actor_user_id = id_from_low<core::UserId>(999),
          .actor_session_id = "sample-test-session",
          .request_id = "sample-test-request",
          .reason = "sample repository test",
      };
    }

    // Mutation context whose actor is a real (seeded) user. CheckoutEvent.user_id
    // is a foreign key to users, so apply_checkout — which stamps the actor as the
    // event user — requires the actor to exist.
    [[nodiscard]] MutationContext mutation_context_as(core::UserId actor) {
      auto context = mutation_context();
      context.actor_user_id = actor;
      return context;
    }

    [[nodiscard]] core::Lab make_lab(std::uint64_t low_bits) {
      return core::Lab{
          .id = id_from_low<core::LabId>(low_bits),
          .name = "Lab " + std::to_string(low_bits),
          .contact = "lab@example.org",
          .created_at = ts(100 + static_cast<std::int64_t>(low_bits)),
          .settings_json = nlohmann::json::object(),
      };
    }

    [[nodiscard]] core::User make_user(std::uint64_t low_bits, core::LabId lab_id) {
      return core::User{
          .id = id_from_low<core::UserId>(low_bits),
          .primary_email = "user" + std::to_string(low_bits) + "@example.org",
          .display_name = "Test User",
          .status = core::UserStatus::Active,
          .created_at = ts(200 + static_cast<std::int64_t>(low_bits)),
          .auth_bindings = nlohmann::json::array({{{"provider", "local"}}}),
          .default_lab_id = lab_id,
      };
    }

    [[nodiscard]] core::ItemType make_item_type(std::uint64_t low_bits, core::LabId lab_id,
                                                std::string name = "liquid") {
      return core::ItemType{
          .id = id_from_low<core::ItemTypeId>(low_bits),
          .lab_id = lab_id,
          .parent_id = std::nullopt,
          .name = std::move(name),
          .created_at = ts(300 + static_cast<std::int64_t>(low_bits)),
      };
    }

    [[nodiscard]] core::ContainerType
    make_container_type(std::uint64_t low_bits, core::LabId lab_id, std::string size_class) {
      return core::ContainerType{
          .id = id_from_low<core::ContainerTypeId>(low_bits),
          .lab_id = lab_id,
          .name = "Container " + std::to_string(low_bits),
          .size_class = std::move(size_class),
          .material = "polypropylene",
          .supplier_sku = "SKU-" + std::to_string(low_bits),
          .created_at = ts(400 + static_cast<std::int64_t>(low_bits)),
      };
    }

    [[nodiscard]] core::BoxType make_box_type(std::uint64_t low_bits, core::LabId lab_id,
                                              const std::string& size_class) {
      return core::BoxType{
          .id = id_from_low<core::BoxTypeId>(low_bits),
          .lab_id = lab_id,
          .name = "BoxType " + std::to_string(low_bits),
          .manufacturer = "Generic",
          .sku = "BT-" + std::to_string(low_bits),
          .positions = {core::Position{.label = "A1", .row = 0, .col = 0, .accepts = {size_class}},
                        core::Position{.label = "A2", .row = 0, .col = 1, .accepts = {size_class}}},
          .created_at = ts(500 + static_cast<std::int64_t>(low_bits)),
      };
    }

    [[nodiscard]] core::StorageContainer make_storage_container(std::uint64_t low_bits,
                                                                core::LabId lab_id) {
      return core::StorageContainer{
          .id = id_from_low<core::StorageContainerId>(low_bits),
          .lab_id = lab_id,
          .parent_id = std::nullopt,
          .kind = core::ContainerKind::Shelf,
          .name = "Shelf " + std::to_string(low_bits),
          .ordering_index = static_cast<int>(low_bits),
          .created_at = ts(600 + static_cast<std::int64_t>(low_bits)),
      };
    }

    [[nodiscard]] core::Box make_box(std::uint64_t low_bits, core::LabId lab_id,
                                     core::BoxTypeId box_type_id,
                                     core::StorageContainerId storage_container_id) {
      return core::Box{
          .id = id_from_low<core::BoxId>(low_bits),
          .lab_id = lab_id,
          .box_type_id = box_type_id,
          .storage_container_id = storage_container_id,
          .label = "Box " + std::to_string(low_bits),
          .created_at = ts(700 + static_cast<std::int64_t>(low_bits)),
      };
    }

    [[nodiscard]] core::Sample make_sample(std::uint64_t low_bits, core::LabId lab_id,
                                           core::ItemTypeId item_type_id, core::UserId user_id) {
      return core::Sample{
          .id = id_from_low<core::SampleId>(low_bits),
          .lab_id = lab_id,
          .item_type_id = item_type_id,
          .name = "Sample " + std::to_string(low_bits),
          .barcode = std::nullopt,
          .container_type_id = std::nullopt,
          .box_id = std::nullopt,
          .position_label = std::nullopt,
          .volume_value = std::nullopt,
          .volume_unit = std::nullopt,
          .mass_value = std::nullopt,
          .mass_unit = std::nullopt,
          .status = core::SampleStatus::Active,
          .parent_sample_id = std::nullopt,
          .created_by = user_id,
          .created_at = ts(800 + static_cast<std::int64_t>(low_bits)),
          .last_modified_by = user_id,
          .last_modified_at = ts(800 + static_cast<std::int64_t>(low_bits)),
          .custom_fields_json = "{}",
          .phi_fields_enc_json = "{}",
      };
    }

    [[nodiscard]] core::Project make_project(std::uint64_t low_bits, core::LabId lab_id,
                                             core::UserId owner_user_id) {
      return core::Project{
          .id = id_from_low<core::ProjectId>(low_bits),
          .lab_id = lab_id,
          .name = "Project " + std::to_string(low_bits),
          .owner_user_id = owner_user_id,
          .created_at = ts(900 + static_cast<std::int64_t>(low_bits)),
      };
    }

    [[nodiscard]] core::CheckoutEvent make_event(std::uint64_t low_bits, core::SampleId sample_id,
                                                 core::LabId lab_id, core::UserId user_id,
                                                 core::CheckoutAction action) {
      return core::CheckoutEvent{
          .id = id_from_low<core::CheckoutEventId>(low_bits),
          .sample_id = sample_id,
          .lab_id = lab_id,
          .user_id = user_id,
          .action = action,
          .reason = "test event",
          .at = ts(1000 + static_cast<std::int64_t>(low_bits)),
          .volume_delta = std::nullopt,
          .volume_unit = std::nullopt,
          .location_after = std::nullopt,
      };
    }

    [[nodiscard]] core::CustomFieldDefinition make_cfd(std::uint64_t low_bits, core::LabId lab_id,
                                                       std::optional<core::ItemTypeId> item_type_id,
                                                       std::string key, bool required = false) {
      return core::CustomFieldDefinition{
          .id = id_from_low<core::CustomFieldDefinitionId>(low_bits),
          .lab_id = lab_id,
          .scope_kind = core::ScopeKind::Sample,
          .item_type_id = item_type_id,
          .key = std::move(key),
          .label = "Field " + std::to_string(low_bits),
          .data_type = core::FieldDataType::String,
          .required = required,
          .validation_json = "{}",
          .indexed = false,
          .is_phi = false,
          .created_at = ts(1100 + static_cast<std::int64_t>(low_bits)),
      };
    }

    // Full seed result used by tests that need box geometry.
    struct BoxSeedResult {
      core::LabId lab_id;
      core::UserId user_id;
      core::ItemTypeId item_type_id;
      core::ContainerTypeId container_type_id;
      std::string size_class;
      core::BoxTypeId box_type_id;
      core::StorageContainerId storage_container_id;
      core::BoxId box_id;
    };

    class SampleRepositoryTest : public ::testing::TestWithParam<BackendKind> {
    protected:
      void SetUp() override {
        if (GetParam() == BackendKind::Postgres && !postgres_test_url().has_value()) {
          GTEST_SKIP() << "FMGR_TEST_POSTGRES_URL not set; skipping Postgres repository tests";
        }
        harness_ = std::make_unique<RepoBackendHarness>(
            GetParam(),
            [](SqliteBackend& b) {
              register_identity_repositories(b);
              register_layout_repositories(b);
              register_box_geometry_repositories(b);
              register_box_repositories(b);
              register_item_type_repositories(b);
              register_sample_repositories(b);
            },
            [](PostgresBackend& b) {
              register_identity_repositories(b);
              register_layout_repositories(b);
              register_box_geometry_repositories(b);
              register_box_repositories(b);
              register_item_type_repositories(b);
              register_sample_repositories(b);
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

      [[nodiscard]] core::UserId seed_user(std::uint64_t low_bits, core::LabId lab_id) {
        const auto user = make_user(low_bits, lab_id);
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::User>().insert(user, mutation_context());
        txn->commit();
        return user.id;
      }

      [[nodiscard]] core::ItemTypeId seed_item_type(std::uint64_t low_bits, core::LabId lab_id,
                                                    std::string name = "liquid") {
        const auto item_type = make_item_type(low_bits, lab_id, std::move(name));
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::ItemType>().insert(item_type, mutation_context());
        txn->commit();
        return item_type.id;
      }

      [[nodiscard]] BoxSeedResult seed_box_prereqs(std::uint64_t base_low) {
        // Each cross-referencing entity is in its own committed transaction because
        // validation queries committed rows (e.g. BoxType checks ContainerType.size_class).
        const auto lab = make_lab(base_low);
        const auto user = make_user(base_low + 1, lab.id);
        const auto item_type = make_item_type(base_low + 2, lab.id);
        const std::string size_class = "cryovial_2ml";
        const auto container_type = make_container_type(base_low + 3, lab.id, size_class);
        const auto box_type = make_box_type(base_low + 4, lab.id, size_class);
        const auto storage_container = make_storage_container(base_low + 5, lab.id);
        const auto box = make_box(base_low + 6, lab.id, box_type.id, storage_container.id);
        {
          auto txn = backend().begin(IsolationLevel::Serializable);
          txn->repo<core::Lab>().insert(lab, mutation_context());
          txn->repo<core::User>().insert(user, mutation_context());
          txn->repo<core::ItemType>().insert(item_type, mutation_context());
          txn->commit();
        }
        {
          auto txn = backend().begin(IsolationLevel::Serializable);
          txn->repo<core::ContainerType>().insert(container_type, mutation_context());
          txn->commit();
        }
        {
          auto txn = backend().begin(IsolationLevel::Serializable);
          txn->repo<core::BoxType>().insert(box_type, mutation_context());
          txn->commit();
        }
        {
          auto txn = backend().begin(IsolationLevel::Serializable);
          txn->repo<core::StorageContainer>().insert(storage_container, mutation_context());
          txn->commit();
        }
        {
          auto txn = backend().begin(IsolationLevel::Serializable);
          txn->repo<core::Box>().insert(box, mutation_context());
          txn->commit();
        }
        return BoxSeedResult{
            .lab_id = lab.id,
            .user_id = user.id,
            .item_type_id = item_type.id,
            .container_type_id = container_type.id,
            .size_class = size_class,
            .box_type_id = box_type.id,
            .storage_container_id = storage_container.id,
            .box_id = box.id,
        };
      }

    private:
      std::unique_ptr<RepoBackendHarness> harness_;
    };

    // ---- Sample CRUD ----

    TEST_P(SampleRepositoryTest, SampleInsertAndFindById) {
      const auto lab_id = seed_lab(1);
      const auto user_id = seed_user(2, lab_id);
      const auto it_id = seed_item_type(3, lab_id);
      const auto sample = make_sample(10, lab_id, it_id, user_id);

      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Sample>().insert(sample, mutation_context());
        txn->commit();
      }
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        const auto found = txn->repo<core::Sample>().find_by_id(sample.id);
        txn->commit();
        ASSERT_TRUE(found.has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        EXPECT_EQ(found.value(), sample);
      }
    }

    TEST_P(SampleRepositoryTest, SampleDuplicateIdThrowsUniqueViolation) {
      const auto lab_id = seed_lab(1);
      const auto user_id = seed_user(2, lab_id);
      const auto it_id = seed_item_type(3, lab_id);
      const auto sample = make_sample(10, lab_id, it_id, user_id);

      auto txn = backend().begin(IsolationLevel::Serializable);
      txn->repo<core::Sample>().insert(sample, mutation_context());
      EXPECT_THROW(txn->repo<core::Sample>().insert(sample, mutation_context()), UniqueViolation);
      txn->rollback();
    }

    // ---- Cross-lab integrity (Phase 6 residuals) ----

    TEST_P(SampleRepositoryTest, ContainerTypeFromAnotherLabIsRejected) {
      const auto lab_a = seed_lab(1);
      const auto user_a = seed_user(2, lab_a);
      const auto it_a = seed_item_type(3, lab_a);

      // A container type that lives in a different lab.
      const auto lab_b = seed_lab(50);
      const auto ct_b = make_container_type(51, lab_b, "cryovial_2ml");
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::ContainerType>().insert(ct_b, mutation_context());
        txn->commit();
      }

      auto sample = make_sample(10, lab_a, it_a, user_a);
      sample.container_type_id = ct_b.id; // unplaced sample, foreign container type

      auto txn = backend().begin(IsolationLevel::Serializable);
      EXPECT_THROW(txn->repo<core::Sample>().insert(sample, mutation_context()),
                   ConstraintViolation);
      txn->rollback();
    }

    TEST_P(SampleRepositoryTest, ContainerTypeFromSameLabIsAccepted) {
      const auto lab_id = seed_lab(1);
      const auto user_id = seed_user(2, lab_id);
      const auto it_id = seed_item_type(3, lab_id);
      const auto container_type = make_container_type(4, lab_id, "cryovial_2ml");
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::ContainerType>().insert(container_type, mutation_context());
        txn->commit();
      }

      auto sample = make_sample(10, lab_id, it_id, user_id);
      sample.container_type_id = container_type.id; // same-lab, unplaced

      auto txn = backend().begin(IsolationLevel::Serializable);
      EXPECT_NO_THROW(txn->repo<core::Sample>().insert(sample, mutation_context()));
      txn->commit();
    }

    TEST_P(SampleRepositoryTest, MultiHopParentLineageCycleIsRejected) {
      const auto lab_id = seed_lab(1);
      const auto user_id = seed_user(2, lab_id);
      const auto it_id = seed_item_type(3, lab_id);

      // Build a lineage chain A <- B <- C (each committed).
      const auto sample_a = make_sample(10, lab_id, it_id, user_id);
      auto sample_b = make_sample(11, lab_id, it_id, user_id);
      sample_b.parent_sample_id = sample_a.id;
      auto sample_c = make_sample(12, lab_id, it_id, user_id);
      sample_c.parent_sample_id = sample_b.id;
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Sample>().insert(sample_a, mutation_context());
        txn->commit();
      }
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Sample>().insert(sample_b, mutation_context());
        txn->commit();
      }
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Sample>().insert(sample_c, mutation_context());
        txn->commit();
      }

      // Point A at C: A <- B <- C <- A would close a 3-hop cycle.
      auto sample_a_updated = sample_a;
      sample_a_updated.parent_sample_id = sample_c.id;

      auto txn = backend().begin(IsolationLevel::Serializable);
      EXPECT_THROW(txn->repo<core::Sample>().update(sample_a_updated, mutation_context()),
                   ConstraintViolation);
      txn->rollback();
    }

    TEST_P(SampleRepositoryTest, SampleUpdateChangesFields) {
      const auto lab_id = seed_lab(1);
      const auto user_id = seed_user(2, lab_id);
      const auto it_id = seed_item_type(3, lab_id);
      const auto sample = make_sample(10, lab_id, it_id, user_id);

      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Sample>().insert(sample, mutation_context());
        txn->commit();
      }

      auto updated = sample;
      updated.name = "Updated Name";
      updated.volume_value = 100;
      updated.volume_unit = core::VolumeUnit::Microliter;
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Sample>().update(updated, mutation_context());
        txn->commit();
      }
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        const auto found = txn->repo<core::Sample>().find_by_id(sample.id);
        txn->commit();
        ASSERT_TRUE(found.has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        EXPECT_EQ(found.value().name, "Updated Name");
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        EXPECT_EQ(found.value().volume_value, 100);
      }
    }

    TEST_P(SampleRepositoryTest, SampleSoftDeleteSetsTombstoneAndExcludesFromQuery) {
      const auto lab_id = seed_lab(1);
      const auto user_id = seed_user(2, lab_id);
      const auto it_id = seed_item_type(3, lab_id);
      const auto sample = make_sample(10, lab_id, it_id, user_id);

      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Sample>().insert(sample, mutation_context());
        txn->commit();
      }
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Sample>().soft_delete(sample.id, mutation_context());
        txn->commit();
      }

      // Default query excludes tombstoned.
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        const auto results = txn->repo<core::Sample>().query(Query<core::Sample>{});
        txn->commit();
        EXPECT_TRUE(results.empty());
      }
      // include_tombstoned() finds the row.
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        const auto results =
            txn->repo<core::Sample>().query(Query<core::Sample>{}.include_tombstoned());
        txn->commit();
        ASSERT_EQ(results.size(), 1U);
        EXPECT_EQ(results.front().status, core::SampleStatus::Tombstoned);
      }
    }

    TEST_P(SampleRepositoryTest, TombstonedSampleVacatesPosition) {
      // After soft-delete, the position slot is free for another sample.
      const auto& [lab_id, user_id, it_id, ct_id, size_class, bt_id, sc_id, box_id] =
          seed_box_prereqs(100);
      (void)ct_id;
      (void)size_class;

      auto s1 = make_sample(10, lab_id, it_id, user_id);
      s1.box_id = box_id;
      s1.position_label = "A1";

      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Sample>().insert(s1, mutation_context());
        txn->commit();
      }
      // Soft-delete frees the position.
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Sample>().soft_delete(s1.id, mutation_context());
        txn->commit();
      }
      // A second sample can now occupy A1.
      auto s2 = make_sample(11, lab_id, it_id, user_id);
      s2.box_id = box_id;
      s2.position_label = "A1";
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Sample>().insert(s2, mutation_context());
        txn->commit();
      }
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        const auto found = txn->repo<core::Sample>().find_by_id(s2.id);
        txn->commit();
        ASSERT_TRUE(found.has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        EXPECT_EQ(found.value().position_label, "A1");
      }
    }

    // ---- Sample validation ----

    TEST_P(SampleRepositoryTest, SampleEmptyNameThrows) {
      const auto lab_id = seed_lab(1);
      const auto user_id = seed_user(2, lab_id);
      const auto it_id = seed_item_type(3, lab_id);
      auto sample = make_sample(10, lab_id, it_id, user_id);
      sample.name = "";

      auto txn = backend().begin(IsolationLevel::Serializable);
      EXPECT_THROW(txn->repo<core::Sample>().insert(sample, mutation_context()),
                   ConstraintViolation);
      txn->rollback();
    }

    TEST_P(SampleRepositoryTest, SampleBadItemTypeIdThrows) {
      const auto lab_id = seed_lab(1);
      const auto user_id = seed_user(2, lab_id);
      // item_type_id references a non-existent ItemType.
      auto sample = make_sample(10, lab_id, id_from_low<core::ItemTypeId>(9999), user_id);

      auto txn = backend().begin(IsolationLevel::Serializable);
      EXPECT_THROW(txn->repo<core::Sample>().insert(sample, mutation_context()),
                   ConstraintViolation);
      txn->rollback();
    }

    TEST_P(SampleRepositoryTest, SampleBadBoxIdThrows) {
      const auto lab_id = seed_lab(1);
      const auto user_id = seed_user(2, lab_id);
      const auto it_id = seed_item_type(3, lab_id);
      auto sample = make_sample(10, lab_id, it_id, user_id);
      // box_id references a non-existent Box.
      sample.box_id = id_from_low<core::BoxId>(9999);
      sample.position_label = "A1";

      auto txn = backend().begin(IsolationLevel::Serializable);
      EXPECT_THROW(txn->repo<core::Sample>().insert(sample, mutation_context()),
                   ConstraintViolation);
      txn->rollback();
    }

    TEST_P(SampleRepositoryTest, SampleBadPositionLabelThrows) {
      const auto& [lab_id, user_id, it_id, ct_id, size_class, bt_id, sc_id, box_id] =
          seed_box_prereqs(100);
      (void)ct_id;
      (void)size_class;

      auto sample = make_sample(10, lab_id, it_id, user_id);
      sample.box_id = box_id;
      sample.position_label = "Z99"; // does not exist in BoxType

      auto txn = backend().begin(IsolationLevel::Serializable);
      EXPECT_THROW(txn->repo<core::Sample>().insert(sample, mutation_context()),
                   ConstraintViolation);
      txn->rollback();
    }

    TEST_P(SampleRepositoryTest, SampleWrongSizeClassThrows) {
      const auto& [lab_id, user_id, it_id, ct_id, size_class, bt_id, sc_id, box_id] =
          seed_box_prereqs(100);
      (void)size_class;
      (void)bt_id;

      // Create a second ContainerType with a different size_class.
      const auto wrong_ct = make_container_type(200, lab_id, "tube_50ml");
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::ContainerType>().insert(wrong_ct, mutation_context());
        txn->commit();
      }

      auto sample = make_sample(10, lab_id, it_id, user_id);
      sample.box_id = box_id;
      sample.position_label = "A1";
      sample.container_type_id = wrong_ct.id; // size_class "tube_50ml" not accepted at A1

      auto txn = backend().begin(IsolationLevel::Serializable);
      EXPECT_THROW(txn->repo<core::Sample>().insert(sample, mutation_context()),
                   ConstraintViolation);
      txn->rollback();
    }

    TEST_P(SampleRepositoryTest, SampleCorrectSizeClassSucceeds) {
      const auto& [lab_id, user_id, it_id, ct_id, size_class, bt_id, sc_id, box_id] =
          seed_box_prereqs(100);
      (void)bt_id;
      (void)size_class;

      auto sample = make_sample(10, lab_id, it_id, user_id);
      sample.box_id = box_id;
      sample.position_label = "A1";
      sample.container_type_id = ct_id; // size_class matches A1 accepts list

      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Sample>().insert(sample, mutation_context());
        txn->commit();
      }
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        const auto found = txn->repo<core::Sample>().find_by_id(sample.id);
        txn->commit();
        ASSERT_TRUE(found.has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        EXPECT_EQ(found.value().container_type_id, ct_id);
      }
    }

    // ---- No-double-booking invariant ----

    TEST_P(SampleRepositoryTest, NoDoubleBooking) {
      // Two active samples cannot occupy the same box + position.
      const auto& [lab_id, user_id, it_id, ct_id, size_class, bt_id, sc_id, box_id] =
          seed_box_prereqs(100);
      (void)ct_id;
      (void)size_class;

      auto s1 = make_sample(10, lab_id, it_id, user_id);
      s1.box_id = box_id;
      s1.position_label = "A1";

      auto s2 = make_sample(11, lab_id, it_id, user_id);
      s2.box_id = box_id;
      s2.position_label = "A1";

      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Sample>().insert(s1, mutation_context());
        txn->commit();
      }
      {
        // SQLite stages and fails the partial unique index at commit; Postgres
        // fails on the immediate insert. Wrap both so either path is caught.
        auto txn = backend().begin(IsolationLevel::Serializable);
        EXPECT_THROW(
            {
              txn->repo<core::Sample>().insert(s2, mutation_context());
              txn->commit();
            },
            UniqueViolation);
      }
    }

    // ---- Atomic sample move (storage::move_sample) ----

    TEST_P(SampleRepositoryTest, MoveToFreePositionVacatesSource) {
      const auto& [lab_id, user_id, it_id, ct_id, size_class, bt_id, sc_id, box_id] =
          seed_box_prereqs(100);
      (void)ct_id;
      (void)size_class;

      auto sample = make_sample(10, lab_id, it_id, user_id);
      sample.box_id = box_id;
      sample.position_label = "A1";
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Sample>().insert(sample, mutation_context());
        txn->commit();
      }
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        move_sample(*txn, sample.id, box_id, "A2", mutation_context());
        txn->commit();
      }
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        const auto moved = txn->repo<core::Sample>().find_by_id(sample.id);
        txn->commit();
        ASSERT_TRUE(moved.has_value());
        // NOLINTBEGIN(bugprone-unchecked-optional-access): guarded by ASSERT_TRUE above
        EXPECT_EQ(moved->box_id, box_id);
        EXPECT_EQ(moved->position_label, "A2");
        // NOLINTEND(bugprone-unchecked-optional-access)
      }
      // A1 is now free: another sample can take it.
      auto other = make_sample(11, lab_id, it_id, user_id);
      other.box_id = box_id;
      other.position_label = "A1";
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        EXPECT_NO_THROW({
          txn->repo<core::Sample>().insert(other, mutation_context());
          txn->commit();
        });
      }
    }

    TEST_P(SampleRepositoryTest, MoveToOccupiedPositionThrowsAndRollsBack) {
      const auto& [lab_id, user_id, it_id, ct_id, size_class, bt_id, sc_id, box_id] =
          seed_box_prereqs(100);
      (void)ct_id;
      (void)size_class;

      auto s1 = make_sample(10, lab_id, it_id, user_id);
      s1.box_id = box_id;
      s1.position_label = "A1";
      auto s2 = make_sample(11, lab_id, it_id, user_id);
      s2.box_id = box_id;
      s2.position_label = "A2";
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Sample>().insert(s1, mutation_context());
        txn->repo<core::Sample>().insert(s2, mutation_context());
        txn->commit();
      }
      {
        // SQLite fails at commit (deferred index), Postgres on the update; either
        // way the transaction rolls back as a unit.
        auto txn = backend().begin(IsolationLevel::Serializable);
        EXPECT_THROW(
            {
              move_sample(*txn, s1.id, box_id, "A2", mutation_context());
              txn->commit();
            },
            UniqueViolation);
      }
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        const auto found1 = txn->repo<core::Sample>().find_by_id(s1.id);
        const auto found2 = txn->repo<core::Sample>().find_by_id(s2.id);
        txn->commit();
        ASSERT_TRUE(found1.has_value());
        ASSERT_TRUE(found2.has_value());
        // NOLINTBEGIN(bugprone-unchecked-optional-access): guarded by ASSERT_TRUE above
        EXPECT_EQ(found1->position_label, "A1"); // unchanged
        EXPECT_EQ(found2->position_label, "A2"); // unchanged
        // NOLINTEND(bugprone-unchecked-optional-access)
      }
    }

    TEST_P(SampleRepositoryTest, MoveToInvalidPositionThrows) {
      const auto& [lab_id, user_id, it_id, ct_id, size_class, bt_id, sc_id, box_id] =
          seed_box_prereqs(100);
      (void)ct_id;
      (void)size_class;

      auto sample = make_sample(10, lab_id, it_id, user_id);
      sample.box_id = box_id;
      sample.position_label = "A1";
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Sample>().insert(sample, mutation_context());
        txn->commit();
      }
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        EXPECT_THROW(
            {
              move_sample(*txn, sample.id, box_id, "Z9", mutation_context());
              txn->commit();
            },
            ConstraintViolation);
      }
    }

    TEST_P(SampleRepositoryTest, MoveNonexistentThrowsNotFound) {
      const auto& [lab_id, user_id, it_id, ct_id, size_class, bt_id, sc_id, box_id] =
          seed_box_prereqs(100);
      (void)lab_id;
      (void)user_id;
      (void)it_id;
      (void)ct_id;
      (void)size_class;
      auto txn = backend().begin(IsolationLevel::Serializable);
      EXPECT_THROW(
          move_sample(*txn, id_from_low<core::SampleId>(999), box_id, "A1", mutation_context()),
          NotFound);
      txn->rollback();
    }

    TEST_P(SampleRepositoryTest, MoveUnplacesWhenDestinationNull) {
      const auto& [lab_id, user_id, it_id, ct_id, size_class, bt_id, sc_id, box_id] =
          seed_box_prereqs(100);
      (void)ct_id;
      (void)size_class;

      auto sample = make_sample(10, lab_id, it_id, user_id);
      sample.box_id = box_id;
      sample.position_label = "A1";
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Sample>().insert(sample, mutation_context());
        txn->commit();
      }
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        move_sample(*txn, sample.id, std::nullopt, std::nullopt, mutation_context());
        txn->commit();
      }
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        const auto unplaced = txn->repo<core::Sample>().find_by_id(sample.id);
        txn->commit();
        ASSERT_TRUE(unplaced.has_value());
        // NOLINTBEGIN(bugprone-unchecked-optional-access): guarded by ASSERT_TRUE above
        EXPECT_FALSE(unplaced->box_id.has_value());
        EXPECT_FALSE(unplaced->position_label.has_value());
        // NOLINTEND(bugprone-unchecked-optional-access)
      }
    }

    TEST_P(SampleRepositoryTest, DifferentPositionsSameBoxAllowed) {
      const auto& [lab_id, user_id, it_id, ct_id, size_class, bt_id, sc_id, box_id] =
          seed_box_prereqs(100);
      (void)ct_id;
      (void)size_class;

      auto s1 = make_sample(10, lab_id, it_id, user_id);
      s1.box_id = box_id;
      s1.position_label = "A1";

      auto s2 = make_sample(11, lab_id, it_id, user_id);
      s2.box_id = box_id;
      s2.position_label = "A2";

      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Sample>().insert(s1, mutation_context());
        txn->repo<core::Sample>().insert(s2, mutation_context());
        txn->commit();
      }
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        EXPECT_TRUE(txn->repo<core::Sample>().find_by_id(s1.id).has_value());
        EXPECT_TRUE(txn->repo<core::Sample>().find_by_id(s2.id).has_value());
        txn->commit();
      }
    }

    // ---- Parent-child lineage ----

    TEST_P(SampleRepositoryTest, ParentChildLineagePreservedAfterParentTombstone) {
      const auto lab_id = seed_lab(1);
      const auto user_id = seed_user(2, lab_id);
      const auto it_id = seed_item_type(3, lab_id);

      const auto parent = make_sample(10, lab_id, it_id, user_id);

      auto child = make_sample(11, lab_id, it_id, user_id);
      child.parent_sample_id = parent.id;

      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Sample>().insert(parent, mutation_context());
        txn->repo<core::Sample>().insert(child, mutation_context());
        txn->commit();
      }
      // Soft-delete parent.
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Sample>().soft_delete(parent.id, mutation_context());
        txn->commit();
      }
      // Child still references the parent via parent_sample_id.
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        const auto found_child = txn->repo<core::Sample>().find_by_id(child.id);
        txn->commit();
        ASSERT_TRUE(found_child.has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        EXPECT_EQ(found_child.value().parent_sample_id, parent.id);
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        EXPECT_EQ(found_child.value().status, core::SampleStatus::Active);
      }
    }

    // ---- Project CRUD ----

    TEST_P(SampleRepositoryTest, ProjectInsertFindUpdateSoftDelete) {
      const auto lab_id = seed_lab(1);
      const auto user_id = seed_user(2, lab_id);
      const auto project = make_project(10, lab_id, user_id);

      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Project>().insert(project, mutation_context());
        txn->commit();
      }
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        const auto found = txn->repo<core::Project>().find_by_id(project.id);
        txn->commit();
        ASSERT_TRUE(found.has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        EXPECT_EQ(found.value(), project);
      }

      // Soft-delete hides from default query.
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Project>().soft_delete(project.id, mutation_context());
        txn->commit();
      }
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        const auto results = txn->repo<core::Project>().query(Query<core::Project>{});
        txn->commit();
        EXPECT_TRUE(results.empty());
      }
      // include_tombstoned finds it.
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        const auto results =
            txn->repo<core::Project>().query(Query<core::Project>{}.include_tombstoned());
        txn->commit();
        ASSERT_EQ(results.size(), 1U);
        EXPECT_TRUE(results.front().archived_at.has_value());
      }
    }

    TEST_P(SampleRepositoryTest, ProjectDuplicateNameInSameLabThrows) {
      const auto lab_id = seed_lab(1);
      const auto user_id = seed_user(2, lab_id);
      const auto p1 = make_project(10, lab_id, user_id);
      auto p2 = make_project(11, lab_id, user_id);
      p2.name = p1.name; // same name, same lab

      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Project>().insert(p1, mutation_context());
        txn->commit();
      }
      {
        // Unique projects(lab_id, name) index: SQLite fails at commit, Postgres
        // on the immediate insert. Wrap both.
        auto txn = backend().begin(IsolationLevel::Serializable);
        EXPECT_THROW(
            {
              txn->repo<core::Project>().insert(p2, mutation_context());
              txn->commit();
            },
            UniqueViolation);
      }
    }

    // ---- SampleProject ----

    TEST_P(SampleRepositoryTest, SampleProjectInsertFindQueryAndHardDelete) {
      const auto lab_id = seed_lab(1);
      const auto user_id = seed_user(2, lab_id);
      const auto it_id = seed_item_type(3, lab_id);
      const auto sample = make_sample(10, lab_id, it_id, user_id);
      const auto project = make_project(20, lab_id, user_id);

      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Sample>().insert(sample, mutation_context());
        txn->repo<core::Project>().insert(project, mutation_context());
        txn->commit();
      }

      const core::SampleProject sp{.sample_id = sample.id, .project_id = project.id};
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::SampleProject>().insert(sp, mutation_context());
        txn->commit();
      }

      // find_by_id
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        const auto found = txn->repo<core::SampleProject>().find_by_id(sp.id());
        txn->commit();
        ASSERT_TRUE(found.has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        EXPECT_EQ(found.value(), sp);
      }

      // query by sample_id
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        const auto results =
            txn->repo<core::SampleProject>().query(Query<core::SampleProject>::where(
                field<core::SampleProject, std::string>(core::SampleProject::Field::SampleId) ==
                sample.id.to_string()));
        txn->commit();
        ASSERT_EQ(results.size(), 1U);
        EXPECT_EQ(results.front().project_id, project.id);
      }

      // soft_delete (hard-deletes the link)
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::SampleProject>().soft_delete(sp.id(), mutation_context());
        txn->commit();
      }
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        const auto found = txn->repo<core::SampleProject>().find_by_id(sp.id());
        txn->commit();
        EXPECT_FALSE(found.has_value());
      }
    }

    TEST_P(SampleRepositoryTest, SampleProjectDuplicateLinkThrows) {
      const auto lab_id = seed_lab(1);
      const auto user_id = seed_user(2, lab_id);
      const auto it_id = seed_item_type(3, lab_id);
      const auto sample = make_sample(10, lab_id, it_id, user_id);
      const auto project = make_project(20, lab_id, user_id);

      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Sample>().insert(sample, mutation_context());
        txn->repo<core::Project>().insert(project, mutation_context());
        txn->commit();
      }

      const core::SampleProject sp{.sample_id = sample.id, .project_id = project.id};
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::SampleProject>().insert(sp, mutation_context());
        txn->commit();
      }
      // Second insert of same link throws UniqueViolation.
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        EXPECT_THROW(txn->repo<core::SampleProject>().insert(sp, mutation_context()),
                     UniqueViolation);
        txn->rollback();
      }
    }

    TEST_P(SampleRepositoryTest, SampleProjectUpdateThrowsUnsupportedOperation) {
      const auto lab_id = seed_lab(1);
      const auto user_id = seed_user(2, lab_id);
      const auto it_id = seed_item_type(3, lab_id);
      const auto sample = make_sample(10, lab_id, it_id, user_id);
      const auto project = make_project(20, lab_id, user_id);

      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Sample>().insert(sample, mutation_context());
        txn->repo<core::Project>().insert(project, mutation_context());
        txn->commit();
      }

      const core::SampleProject sp{.sample_id = sample.id, .project_id = project.id};
      auto txn = backend().begin(IsolationLevel::Serializable);
      EXPECT_THROW(txn->repo<core::SampleProject>().update(sp, mutation_context()),
                   UnsupportedOperation);
      txn->rollback();
    }

    // ---- CheckoutEvent ----

    TEST_P(SampleRepositoryTest, CheckoutEventInsertAndFindById) {
      const auto lab_id = seed_lab(1);
      const auto user_id = seed_user(2, lab_id);
      const auto it_id = seed_item_type(3, lab_id);
      const auto sample = make_sample(10, lab_id, it_id, user_id);

      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Sample>().insert(sample, mutation_context());
        txn->commit();
      }

      const auto event =
          make_event(50, sample.id, lab_id, user_id, core::CheckoutAction::CheckedOut);
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::CheckoutEvent>().insert(event, mutation_context());
        txn->commit();
      }
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        const auto found = txn->repo<core::CheckoutEvent>().find_by_id(event.id);
        txn->commit();
        ASSERT_TRUE(found.has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        EXPECT_EQ(found.value(), event);
      }
    }

    TEST_P(SampleRepositoryTest, CheckoutEventQueryBySampleId) {
      const auto lab_id = seed_lab(1);
      const auto user_id = seed_user(2, lab_id);
      const auto it_id = seed_item_type(3, lab_id);
      const auto s1 = make_sample(10, lab_id, it_id, user_id);
      const auto s2 = make_sample(11, lab_id, it_id, user_id);

      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Sample>().insert(s1, mutation_context());
        txn->repo<core::Sample>().insert(s2, mutation_context());
        txn->commit();
      }

      const auto e1 = make_event(50, s1.id, lab_id, user_id, core::CheckoutAction::CheckedOut);
      const auto e2 = make_event(51, s1.id, lab_id, user_id, core::CheckoutAction::CheckedIn);
      const auto e3 = make_event(52, s2.id, lab_id, user_id, core::CheckoutAction::CheckedOut);
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::CheckoutEvent>().insert(e1, mutation_context());
        txn->repo<core::CheckoutEvent>().insert(e2, mutation_context());
        txn->repo<core::CheckoutEvent>().insert(e3, mutation_context());
        txn->commit();
      }
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        const auto results =
            txn->repo<core::CheckoutEvent>().query(Query<core::CheckoutEvent>::where(
                field<core::CheckoutEvent, std::string>(core::CheckoutEvent::Field::SampleId) ==
                s1.id.to_string()));
        txn->commit();
        EXPECT_EQ(results.size(), 2U);
      }
    }

    TEST_P(SampleRepositoryTest, CheckoutEventUpdateThrowsUnsupportedOperation) {
      const auto lab_id = seed_lab(1);
      const auto user_id = seed_user(2, lab_id);
      const auto it_id = seed_item_type(3, lab_id);
      const auto sample = make_sample(10, lab_id, it_id, user_id);

      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Sample>().insert(sample, mutation_context());
        txn->commit();
      }

      const auto event =
          make_event(50, sample.id, lab_id, user_id, core::CheckoutAction::CheckedOut);
      auto txn = backend().begin(IsolationLevel::Serializable);
      EXPECT_THROW(txn->repo<core::CheckoutEvent>().update(event, mutation_context()),
                   UnsupportedOperation);
      txn->rollback();
    }

    TEST_P(SampleRepositoryTest, CheckoutEventSoftDeleteThrowsUnsupportedOperation) {
      const auto lab_id = seed_lab(1);
      const auto user_id = seed_user(2, lab_id);
      const auto it_id = seed_item_type(3, lab_id);
      const auto sample = make_sample(10, lab_id, it_id, user_id);

      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Sample>().insert(sample, mutation_context());
        txn->commit();
      }

      const auto event_id = id_from_low<core::CheckoutEventId>(50);
      auto txn = backend().begin(IsolationLevel::Serializable);
      EXPECT_THROW(txn->repo<core::CheckoutEvent>().soft_delete(event_id, mutation_context()),
                   UnsupportedOperation);
      txn->rollback();
    }

    // ---- Phase 2: Query predicates ----

    TEST_P(SampleRepositoryTest, SampleQuerySortByCreatedAt) {
      const auto lab_id = seed_lab(1);
      const auto user_id = seed_user(2, lab_id);
      const auto it_id = seed_item_type(3, lab_id);

      auto s1 = make_sample(10, lab_id, it_id, user_id);
      s1.created_at = ts(100);
      s1.last_modified_at = ts(100);

      auto s2 = make_sample(11, lab_id, it_id, user_id);
      s2.created_at = ts(200);
      s2.last_modified_at = ts(200);

      auto s3 = make_sample(12, lab_id, it_id, user_id);
      s3.created_at = ts(300);
      s3.last_modified_at = ts(300);

      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Sample>().insert(s1, mutation_context());
        txn->repo<core::Sample>().insert(s2, mutation_context());
        txn->repo<core::Sample>().insert(s3, mutation_context());
        txn->commit();
      }
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        const auto results = txn->repo<core::Sample>().query(Query<core::Sample>::all().order_by(
            field<core::Sample, std::int64_t>(core::Sample::Field::CreatedAt)));
        txn->commit();
        ASSERT_EQ(results.size(), 3U);
        EXPECT_EQ(results[0].created_at, ts(100));
        EXPECT_EQ(results[1].created_at, ts(200));
        EXPECT_EQ(results[2].created_at, ts(300));
      }
    }

    TEST_P(SampleRepositoryTest, SampleQueryLimitAndOffset) {
      const auto lab_id = seed_lab(1);
      const auto user_id = seed_user(2, lab_id);
      const auto it_id = seed_item_type(3, lab_id);

      auto s1 = make_sample(10, lab_id, it_id, user_id);
      s1.name = "S1";
      auto s2 = make_sample(11, lab_id, it_id, user_id);
      s2.name = "S2";
      auto s3 = make_sample(12, lab_id, it_id, user_id);
      s3.name = "S3";
      auto s4 = make_sample(13, lab_id, it_id, user_id);
      s4.name = "S4";
      auto s5 = make_sample(14, lab_id, it_id, user_id);
      s5.name = "S5";

      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Sample>().insert(s1, mutation_context());
        txn->repo<core::Sample>().insert(s2, mutation_context());
        txn->repo<core::Sample>().insert(s3, mutation_context());
        txn->repo<core::Sample>().insert(s4, mutation_context());
        txn->repo<core::Sample>().insert(s5, mutation_context());
        txn->commit();
      }
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        const auto results =
            txn->repo<core::Sample>().query(Query<core::Sample>::all().limit(2).offset(1));
        txn->commit();
        EXPECT_EQ(results.size(), 2U);
      }
    }

    TEST_P(SampleRepositoryTest, SampleQueryAndWhereMultiplePredicates) {
      const auto lab_id = seed_lab(1);
      const auto user_id = seed_user(2, lab_id);
      const auto it_id = seed_item_type(3, lab_id);

      auto s1 = make_sample(10, lab_id, it_id, user_id);
      s1.status = core::SampleStatus::Active;

      auto s2 = make_sample(11, lab_id, it_id, user_id);
      s2.status = core::SampleStatus::Tombstoned;

      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Sample>().insert(s1, mutation_context());
        txn->repo<core::Sample>().insert(s2, mutation_context());
        txn->commit();
      }
      const std::string active_str("active");
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        const auto results = txn->repo<core::Sample>().query(
            Query<core::Sample>::where(
                field<core::Sample, std::string>(core::Sample::Field::LabId) == lab_id.to_string())
                .and_where(field<core::Sample, std::string>(core::Sample::Field::Status) ==
                           active_str));
        txn->commit();
        ASSERT_EQ(results.size(), 1U);
        EXPECT_EQ(results.front().status, core::SampleStatus::Active);
      }
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        const auto results = txn->repo<core::Sample>().query(
            Query<core::Sample>::where(
                field<core::Sample, std::string>(core::Sample::Field::LabId) == lab_id.to_string())
                .include_tombstoned());
        txn->commit();
        EXPECT_EQ(results.size(), 2U);
      }
    }

    TEST_P(SampleRepositoryTest, SampleFindByNonexistentIdReturnsEmpty) {
      auto txn = backend().begin(IsolationLevel::Serializable);
      const auto found = txn->repo<core::Sample>().find_by_id(id_from_low<core::SampleId>(99999));
      txn->commit();
      EXPECT_FALSE(found.has_value());
    }

    TEST_P(SampleRepositoryTest, CheckoutEventQuerySortByAtDescending) {
      const auto lab_id = seed_lab(1);
      const auto user_id = seed_user(2, lab_id);
      const auto it_id = seed_item_type(3, lab_id);
      const auto sample = make_sample(10, lab_id, it_id, user_id);

      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Sample>().insert(sample, mutation_context());
        txn->commit();
      }

      auto e1 = make_event(50, sample.id, lab_id, user_id, core::CheckoutAction::CheckedOut);
      e1.at = ts(100);
      auto e2 = make_event(51, sample.id, lab_id, user_id, core::CheckoutAction::CheckedIn);
      e2.at = ts(200);
      auto e3 = make_event(52, sample.id, lab_id, user_id, core::CheckoutAction::CheckedOut);
      e3.at = ts(300);

      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::CheckoutEvent>().insert(e1, mutation_context());
        txn->repo<core::CheckoutEvent>().insert(e2, mutation_context());
        txn->repo<core::CheckoutEvent>().insert(e3, mutation_context());
        txn->commit();
      }
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        const auto results =
            txn->repo<core::CheckoutEvent>().query(Query<core::CheckoutEvent>::all().order_by(
                field<core::CheckoutEvent, std::int64_t>(core::CheckoutEvent::Field::At),
                SortDirection::Descending));
        txn->commit();
        ASSERT_FALSE(results.empty());
        EXPECT_EQ(results.front().at, ts(300));
      }
    }

    // ---- Phase 4: Audit ----

    TEST_P(SampleRepositoryTest, SampleUpdateAppendsAuditEvent) {
      const auto lab_id = seed_lab(1);
      const auto user_id = seed_user(2, lab_id);
      const auto it_id = seed_item_type(3, lab_id);
      const auto sample = make_sample(10, lab_id, it_id, user_id);

      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Sample>().insert(sample, mutation_context());
        txn->commit();
      }

      const auto baseline = audit_event_count();

      auto updated = sample;
      updated.name = "Updated Name";

      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Sample>().update(updated, mutation_context());
        txn->commit();
      }

      EXPECT_EQ(audit_event_count(), baseline + 1U);
    }

    TEST_P(SampleRepositoryTest, SampleSoftDeleteAppendsAuditEvent) {
      const auto lab_id = seed_lab(1);
      const auto user_id = seed_user(2, lab_id);
      const auto it_id = seed_item_type(3, lab_id);
      const auto sample = make_sample(10, lab_id, it_id, user_id);

      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Sample>().insert(sample, mutation_context());
        txn->commit();
      }

      const auto baseline = audit_event_count();

      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Sample>().soft_delete(sample.id, mutation_context());
        txn->commit();
      }

      EXPECT_EQ(audit_event_count(), baseline + 1U);
    }

    TEST_P(SampleRepositoryTest, MultipleMutationsInOneTransactionAppendMultipleAuditEvents) {
      const auto lab_id = seed_lab(1);
      const auto user_id = seed_user(2, lab_id);
      const auto it_id = seed_item_type(3, lab_id);

      const auto baseline = audit_event_count();

      const auto s1 = make_sample(10, lab_id, it_id, user_id);
      const auto s2 = make_sample(11, lab_id, it_id, user_id);
      const auto s3 = make_sample(12, lab_id, it_id, user_id);

      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Sample>().insert(s1, mutation_context());
        txn->repo<core::Sample>().insert(s2, mutation_context());
        txn->repo<core::Sample>().insert(s3, mutation_context());
        txn->commit();
      }

      EXPECT_EQ(audit_event_count(), baseline + 3U);
    }

    // ---- Phase 5: Concurrency ----

    TEST_P(SampleRepositoryTest, ConcurrentSampleUpdateSerializesConflicts) {
      // This last-writer-wins scenario relies on SQLite's deferred-write staging:
      // an uncommitted update holds no row lock, so a second concurrent
      // transaction can update and commit first. On Postgres the immediate UPDATE
      // takes a row lock, so the interleaving below would deadlock the single
      // thread. The cross-transaction serialization guarantee itself is covered by
      // the Postgres conformance suite.
      if (GetParam() == BackendKind::Postgres) {
        GTEST_SKIP() << "relies on SQLite deferred-write semantics";
      }
      const auto lab_id = seed_lab(1);
      const auto user_id = seed_user(2, lab_id);
      const auto it_id = seed_item_type(3, lab_id);
      auto sample = make_sample(10, lab_id, it_id, user_id);
      sample.name = "original";

      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Sample>().insert(sample, mutation_context());
        txn->commit();
      }

      auto updated_a = sample;
      updated_a.name = "alice";

      auto updated_b = sample;
      updated_b.name = "bob";

      auto txn_a = backend().begin(IsolationLevel::Serializable);
      const auto read_a = txn_a->repo<core::Sample>().find_by_id(sample.id);
      ASSERT_TRUE(read_a.has_value());
      txn_a->repo<core::Sample>().update(updated_a, mutation_context());
      // Do NOT commit txn_a yet.

      {
        auto txn_b = backend().begin(IsolationLevel::Serializable);
        const auto read_b = txn_b->repo<core::Sample>().find_by_id(sample.id);
        ASSERT_TRUE(read_b.has_value());
        txn_b->repo<core::Sample>().update(updated_b, mutation_context());
        txn_b->commit();
      }

      // Last writer wins: txn_a commits after txn_b.
      EXPECT_NO_THROW(txn_a->commit());

      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        const auto final_result = txn->repo<core::Sample>().find_by_id(sample.id);
        txn->commit();
        ASSERT_TRUE(final_result.has_value());
        EXPECT_EQ(final_result->name, "alice");
      }
    }

    TEST_P(SampleRepositoryTest, ConcurrentSoftDeleteAndReadShowsSnapshotIsolation) {
      const auto lab_id = seed_lab(1);
      const auto user_id = seed_user(2, lab_id);
      const auto it_id = seed_item_type(3, lab_id);
      const auto s1 = make_sample(10, lab_id, it_id, user_id);

      // Txn A: insert s1 and commit.
      {
        auto txn_a = backend().begin(IsolationLevel::Serializable);
        txn_a->repo<core::Sample>().insert(s1, mutation_context());
        txn_a->commit();
      }

      // Verify s1 is visible in a new transaction.
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        const auto results = txn->repo<core::Sample>().query(Query<core::Sample>::all());
        txn->commit();
        ASSERT_EQ(results.size(), 1U);
      }

      // Txn A: soft_delete s1 and commit.
      {
        auto txn_a = backend().begin(IsolationLevel::Serializable);
        const auto read_a = txn_a->repo<core::Sample>().find_by_id(s1.id);
        ASSERT_TRUE(read_a.has_value());
        txn_a->repo<core::Sample>().soft_delete(s1.id, mutation_context());
        txn_a->commit();
      }

      // After soft-delete commits, default query returns empty.
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        const auto results = txn->repo<core::Sample>().query(Query<core::Sample>::all());
        txn->commit();
        EXPECT_TRUE(results.empty());
      }

      // include_tombstoned finds the soft-deleted sample.
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        const auto results =
            txn->repo<core::Sample>().query(Query<core::Sample>::all().include_tombstoned());
        txn->commit();
        ASSERT_EQ(results.size(), 1U);
        EXPECT_EQ(results.front().status, core::SampleStatus::Tombstoned);
      }
    }

    TEST_P(SampleRepositoryTest, ConcurrentCheckoutEventInsertsBothSucceed) {
      const auto lab_id = seed_lab(1);
      const auto user_id = seed_user(2, lab_id);
      const auto it_id = seed_item_type(3, lab_id);
      const auto sample = make_sample(10, lab_id, it_id, user_id);

      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Sample>().insert(sample, mutation_context());
        txn->commit();
      }

      const auto e1 = make_event(50, sample.id, lab_id, user_id, core::CheckoutAction::CheckedOut);
      const auto e2 = make_event(51, sample.id, lab_id, user_id, core::CheckoutAction::CheckedIn);

      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::CheckoutEvent>().insert(e1, mutation_context());
        txn->repo<core::CheckoutEvent>().insert(e2, mutation_context());
        txn->commit();
      }

      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        const auto results =
            txn->repo<core::CheckoutEvent>().query(Query<core::CheckoutEvent>::where(
                field<core::CheckoutEvent, std::string>(core::CheckoutEvent::Field::SampleId) ==
                sample.id.to_string()));
        txn->commit();
        EXPECT_EQ(results.size(), 2U);
      }
    }

    TEST_P(SampleRepositoryTest, SampleUpdateOnNonexistentThrows) {
      const auto lab_id = seed_lab(1);
      const auto user_id = seed_user(2, lab_id);
      const auto it_id = seed_item_type(3, lab_id);
      const auto s = make_sample(99999, lab_id, it_id, user_id);

      auto txn = backend().begin(IsolationLevel::Serializable);
      EXPECT_THROW(txn->repo<core::Sample>().update(s, mutation_context()), NotFound);
      txn->rollback();
    }

    TEST_P(SampleRepositoryTest, SampleQueryBetweenCreatedAt) {
      const auto lab_id = seed_lab(1);
      const auto user_id = seed_user(2, lab_id);
      const auto it_id = seed_item_type(3, lab_id);

      auto s1 = make_sample(10, lab_id, it_id, user_id);
      s1.created_at = ts(100);
      s1.last_modified_at = ts(100);

      auto s2 = make_sample(11, lab_id, it_id, user_id);
      s2.created_at = ts(150);
      s2.last_modified_at = ts(150);

      auto s3 = make_sample(12, lab_id, it_id, user_id);
      s3.created_at = ts(200);
      s3.last_modified_at = ts(200);

      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Sample>().insert(s1, mutation_context());
        txn->repo<core::Sample>().insert(s2, mutation_context());
        txn->repo<core::Sample>().insert(s3, mutation_context());
        txn->commit();
      }
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        const auto results = txn->repo<core::Sample>().query(Query<core::Sample>::where(
            between(field<core::Sample, std::int64_t>(core::Sample::Field::CreatedAt),
                    ts(120).unix_micros(), ts(180).unix_micros())));
        txn->commit();
        ASSERT_EQ(results.size(), 1U);
        EXPECT_EQ(results.front().created_at, ts(150));
      }
    }

    TEST_P(SampleRepositoryTest, SampleQueryJsonPathOnCustomFields) {
      const auto lab_id = seed_lab(1);
      const auto user_id = seed_user(2, lab_id);
      const auto it_id = seed_item_type(3, lab_id);

      auto s1 = make_sample(10, lab_id, it_id, user_id);
      s1.custom_fields_json = R"({"color":"red","size":"large"})";

      auto s2 = make_sample(11, lab_id, it_id, user_id);
      s2.custom_fields_json = R"({"color":"blue","size":"small"})";

      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Sample>().insert(s1, mutation_context());
        txn->repo<core::Sample>().insert(s2, mutation_context());
        txn->commit();
      }
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        const auto results = txn->repo<core::Sample>().query(Query<core::Sample>::where(
            json_path<core::Sample>(core::Sample::Field::CustomFieldsJson, {"color"}) == "red"));
        txn->commit();
        ASSERT_EQ(results.size(), 1U);
        EXPECT_EQ(results.front().id, s1.id);
      }
    }

    TEST_P(SampleRepositoryTest, ProjectQuerySortByCreatedAt) {
      const auto lab_id = seed_lab(1);
      const auto user_id = seed_user(2, lab_id);

      auto p1 = make_project(10, lab_id, user_id);
      p1.created_at = ts(100);

      auto p2 = make_project(11, lab_id, user_id);
      p2.created_at = ts(200);

      auto p3 = make_project(12, lab_id, user_id);
      p3.created_at = ts(300);

      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Project>().insert(p1, mutation_context());
        txn->repo<core::Project>().insert(p2, mutation_context());
        txn->repo<core::Project>().insert(p3, mutation_context());
        txn->commit();
      }
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        const auto results = txn->repo<core::Project>().query(Query<core::Project>::all().order_by(
            field<core::Project, std::int64_t>(core::Project::Field::CreatedAt)));
        txn->commit();
        ASSERT_EQ(results.size(), 3U);
        EXPECT_EQ(results[0].created_at, ts(100));
        EXPECT_EQ(results[1].created_at, ts(200));
        EXPECT_EQ(results[2].created_at, ts(300));
      }
    }

    TEST_P(SampleRepositoryTest, SampleProjectFindByNonexistentIdReturnsEmpty) {
      auto txn = backend().begin(IsolationLevel::Serializable);
      const auto found = txn->repo<core::SampleProject>().find_by_id(core::SampleProjectId{
          id_from_low<core::SampleId>(1), id_from_low<core::ProjectId>(99999)});
      txn->commit();
      EXPECT_FALSE(found.has_value());
    }

    TEST_P(SampleRepositoryTest, CheckoutEventQueryByTargetUserId) {
      const auto lab_id = seed_lab(1);
      const auto user_id = seed_user(1, lab_id);
      const auto it_id = seed_item_type(3, lab_id);
      const auto sample = make_sample(10, lab_id, it_id, user_id);

      const auto target_user_id_1 = seed_user(2, lab_id);
      const auto target_user_id_2 = seed_user(3, lab_id);

      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Sample>().insert(sample, mutation_context());
        txn->commit();
      }

      auto e1 =
          make_event(50, sample.id, lab_id, target_user_id_1, core::CheckoutAction::CheckedOut);
      auto e2 =
          make_event(51, sample.id, lab_id, target_user_id_2, core::CheckoutAction::CheckedOut);

      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::CheckoutEvent>().insert(e1, mutation_context());
        txn->repo<core::CheckoutEvent>().insert(e2, mutation_context());
        txn->commit();
      }
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        const auto results =
            txn->repo<core::CheckoutEvent>().query(Query<core::CheckoutEvent>::where(
                field<core::CheckoutEvent, std::string>(core::CheckoutEvent::Field::UserId) ==
                target_user_id_1.to_string()));
        txn->commit();
        ASSERT_EQ(results.size(), 1U);
        EXPECT_EQ(results.front().id, e1.id);
      }
    }

    TEST_P(SampleRepositoryTest, InsertSampleWithInvalidParentThrowsForeignKeyViolation) {
      const auto lab_id = seed_lab(1);
      const auto user_id = seed_user(2, lab_id);
      const auto it_id = seed_item_type(3, lab_id);

      auto sample = make_sample(50, lab_id, it_id, user_id);
      sample.parent_sample_id = id_from_low<core::SampleId>(99999); // nonexistent

      // App-level validation catches nonexistent parent at insert time.
      auto txn = backend().begin(IsolationLevel::Serializable);
      EXPECT_THROW(txn->repo<core::Sample>().insert(sample, mutation_context()),
                   ForeignKeyViolation);
    }

    TEST_P(SampleRepositoryTest, InsertSampleWithCrossLabParentThrowsForeignKeyViolation) {
      const auto lab1_id = seed_lab(500);
      const auto user1_id = seed_user(501, lab1_id);
      const auto it1_id = seed_item_type(502, lab1_id);
      const auto lab2_id = seed_lab(510);
      const auto user2_id = seed_user(511, lab2_id);
      const auto it2_id = seed_item_type(512, lab2_id);

      // Insert parent sample in lab2.
      auto parent = make_sample(520, lab2_id, it2_id, user2_id);
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Sample>().insert(parent, mutation_context());
        txn->commit();
      }

      // Insert child in lab1 referencing the lab2 parent — must fail.
      auto child = make_sample(521, lab1_id, it1_id, user1_id);
      child.parent_sample_id = parent.id;

      auto txn = backend().begin(IsolationLevel::Serializable);
      EXPECT_THROW(txn->repo<core::Sample>().insert(child, mutation_context()),
                   ForeignKeyViolation);
    }

    TEST_P(SampleRepositoryTest, InsertSampleWithSelfParentThrowsConstraintViolation) {
      const auto lab_id = seed_lab(530);
      const auto user_id = seed_user(531, lab_id);
      const auto it_id = seed_item_type(532, lab_id);

      auto sample = make_sample(540, lab_id, it_id, user_id);
      sample.parent_sample_id = sample.id; // self-reference

      auto txn = backend().begin(IsolationLevel::Serializable);
      EXPECT_THROW(txn->repo<core::Sample>().insert(sample, mutation_context()),
                   ConstraintViolation);
    }

    TEST_P(SampleRepositoryTest, InsertSampleProjectCrossLabThrowsForeignKeyViolation) {
      const auto lab1_id = seed_lab(600);
      const auto user1_id = seed_user(601, lab1_id);
      const auto it1_id = seed_item_type(602, lab1_id);
      const auto lab2_id = seed_lab(610);
      const auto user2_id = seed_user(611, lab2_id);

      auto sample = make_sample(620, lab1_id, it1_id, user1_id);
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Sample>().insert(sample, mutation_context());
        txn->commit();
      }

      auto project = make_project(630, lab2_id, user2_id); // project in lab2, sample in lab1
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Project>().insert(project, mutation_context());
        txn->commit();
      }

      const core::SampleProject sp{.sample_id = sample.id, .project_id = project.id};
      auto txn = backend().begin(IsolationLevel::Serializable);
      EXPECT_THROW(txn->repo<core::SampleProject>().insert(sp, mutation_context()),
                   ForeignKeyViolation);
    }

    TEST_P(SampleRepositoryTest, InsertCheckoutEventCrossLabThrowsConstraintViolation) {
      const auto lab1_id = seed_lab(700);
      const auto user1_id = seed_user(701, lab1_id);
      const auto it1_id = seed_item_type(702, lab1_id);
      const auto lab2_id = seed_lab(710);
      const auto user2_id = seed_user(711, lab2_id);

      auto sample = make_sample(720, lab1_id, it1_id, user1_id);
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Sample>().insert(sample, mutation_context());
        txn->commit();
      }

      // Event claims lab2 but sample belongs to lab1.
      auto event = make_event(730, sample.id, lab2_id, user2_id, core::CheckoutAction::CheckedOut);
      auto txn = backend().begin(IsolationLevel::Serializable);
      EXPECT_THROW(txn->repo<core::CheckoutEvent>().insert(event, mutation_context()),
                   ConstraintViolation);
    }

    TEST_P(SampleRepositoryTest, UpdateNonexistentSampleThrowsNotFound) {
      auto txn = backend().begin(IsolationLevel::Serializable);
      auto sample = make_sample(9999, id_from_low<core::LabId>(1), id_from_low<core::ItemTypeId>(2),
                                id_from_low<core::UserId>(3));
      EXPECT_THROW(txn->repo<core::Sample>().update(sample, mutation_context()), NotFound);
    }

    // ---- Check-out / check-in / discard (storage::apply_checkout) ----

    TEST_P(SampleRepositoryTest, CheckoutTransitionsActiveToCheckedOutAndAppendsEvent) {
      const auto lab_id = seed_lab(1);
      const auto user_id = seed_user(2, lab_id);
      const auto it_id = seed_item_type(3, lab_id);
      const auto sample = make_sample(10, lab_id, it_id, user_id);
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Sample>().insert(sample, mutation_context());
        txn->commit();
      }

      core::Sample result;
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        result = apply_checkout(*txn, sample.id,
                                CheckoutCommand{.action = core::CheckoutAction::CheckedOut,
                                                .event_id = id_from_low<core::CheckoutEventId>(50),
                                                .at = ts(900)},
                                mutation_context_as(user_id));
        txn->commit();
      }
      EXPECT_EQ(result.status, core::SampleStatus::CheckedOut);
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        const auto events =
            txn->repo<core::CheckoutEvent>().query(Query<core::CheckoutEvent>::where(
                field<core::CheckoutEvent, std::string>(core::CheckoutEvent::Field::SampleId) ==
                sample.id.to_string()));
        txn->commit();
        ASSERT_EQ(events.size(), 1U);
        EXPECT_EQ(events.front().action, core::CheckoutAction::CheckedOut);
      }
    }

    TEST_P(SampleRepositoryTest, CheckinConsumingAllVolumeAutoDepletes) {
      const auto lab_id = seed_lab(1);
      const auto user_id = seed_user(2, lab_id);
      const auto it_id = seed_item_type(3, lab_id);
      auto sample = make_sample(10, lab_id, it_id, user_id);
      sample.volume_value = 100;
      sample.volume_unit = core::VolumeUnit::Microliter;
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Sample>().insert(sample, mutation_context());
        txn->commit();
      }
      // Check out, then check back in consuming the full 100 µL.
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        apply_checkout(*txn, sample.id,
                       CheckoutCommand{.action = core::CheckoutAction::CheckedOut,
                                       .event_id = id_from_low<core::CheckoutEventId>(50),
                                       .at = ts(900)},
                       mutation_context_as(user_id));
        txn->commit();
      }
      core::Sample result;
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        result = apply_checkout(*txn, sample.id,
                                CheckoutCommand{.action = core::CheckoutAction::CheckedIn,
                                                .volume_used = core::Volume::from_raw(
                                                    100, core::VolumeUnit::Microliter),
                                                .event_id = id_from_low<core::CheckoutEventId>(51),
                                                .at = ts(901)},
                                mutation_context_as(user_id));
        txn->commit();
      }
      EXPECT_EQ(result.status, core::SampleStatus::Depleted);
      EXPECT_EQ(result.volume_value, 0);
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        const auto event =
            txn->repo<core::CheckoutEvent>().find_by_id(id_from_low<core::CheckoutEventId>(51));
        txn->commit();
        ASSERT_TRUE(event.has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        EXPECT_EQ(event->volume_delta, -100); // negative = consumed
      }
    }

    TEST_P(SampleRepositoryTest, DiscardConsumesRemainingVolumeAndDestroys) {
      const auto lab_id = seed_lab(1);
      const auto user_id = seed_user(2, lab_id);
      const auto it_id = seed_item_type(3, lab_id);
      auto sample = make_sample(10, lab_id, it_id, user_id);
      sample.volume_value = 50;
      sample.volume_unit = core::VolumeUnit::Microliter;
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Sample>().insert(sample, mutation_context());
        txn->commit();
      }
      core::Sample result;
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        result = apply_checkout(*txn, sample.id,
                                CheckoutCommand{.action = core::CheckoutAction::Destroyed,
                                                .event_id = id_from_low<core::CheckoutEventId>(50),
                                                .at = ts(900)},
                                mutation_context_as(user_id));
        txn->commit();
      }
      EXPECT_EQ(result.status, core::SampleStatus::Destroyed);
      EXPECT_EQ(result.volume_value, 0);
    }

    TEST_P(SampleRepositoryTest, CheckoutOfAlreadyCheckedOutSampleThrows) {
      const auto lab_id = seed_lab(1);
      const auto user_id = seed_user(2, lab_id);
      const auto it_id = seed_item_type(3, lab_id);
      const auto sample = make_sample(10, lab_id, it_id, user_id);
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::Sample>().insert(sample, mutation_context());
        txn->commit();
      }
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        apply_checkout(*txn, sample.id,
                       CheckoutCommand{.action = core::CheckoutAction::CheckedOut,
                                       .event_id = id_from_low<core::CheckoutEventId>(50),
                                       .at = ts(900)},
                       mutation_context_as(user_id));
        txn->commit();
      }
      auto txn = backend().begin(IsolationLevel::Serializable);
      EXPECT_THROW(
          apply_checkout(*txn, sample.id,
                         CheckoutCommand{.action = core::CheckoutAction::CheckedOut,
                                         .event_id = id_from_low<core::CheckoutEventId>(51),
                                         .at = ts(901)},
                         mutation_context_as(user_id)),
          ConstraintViolation);
      txn->rollback();
    }

    // ---- Custom-field definition resolution (storage::resolve_custom_field_defs) ----

    TEST_P(SampleRepositoryTest, ResolveCustomFieldDefsMergesGlobalAndAncestorChain) {
      const auto lab_id = seed_lab(1);
      auto root = make_item_type(3, lab_id, "liquid");
      auto child = make_item_type(4, lab_id, "blood");
      child.parent_id = root.id;
      auto unrelated = make_item_type(5, lab_id, "solid");
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::ItemType>().insert(root, mutation_context());
        txn->repo<core::ItemType>().insert(child, mutation_context());
        txn->repo<core::ItemType>().insert(unrelated, mutation_context());
        txn->commit();
      }
      const auto global = make_cfd(10, lab_id, std::nullopt, "g");
      const auto on_root = make_cfd(11, lab_id, root.id, "a", /*required=*/true);
      const auto on_child = make_cfd(12, lab_id, child.id, "b");
      const auto on_unrelated = make_cfd(13, lab_id, unrelated.id, "x");
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::CustomFieldDefinition>().insert(global, mutation_context());
        txn->repo<core::CustomFieldDefinition>().insert(on_root, mutation_context());
        txn->repo<core::CustomFieldDefinition>().insert(on_child, mutation_context());
        txn->repo<core::CustomFieldDefinition>().insert(on_unrelated, mutation_context());
        txn->commit();
      }

      std::vector<core::CustomFieldDefinition> resolved;
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        resolved = resolve_custom_field_defs(*txn, lab_id, child.id);
        txn->commit();
      }
      std::set<std::string> keys;
      for (const auto& cfd : resolved) {
        keys.insert(cfd.key);
      }
      EXPECT_EQ(keys, (std::set<std::string>{"a", "b", "g"})); // not "x"
    }

    TEST_P(SampleRepositoryTest, ResolveCustomFieldDefsChildOverridesAncestorOnDuplicateKey) {
      const auto lab_id = seed_lab(1);
      auto root = make_item_type(3, lab_id, "liquid");
      auto child = make_item_type(4, lab_id, "blood");
      child.parent_id = root.id;
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::ItemType>().insert(root, mutation_context());
        txn->repo<core::ItemType>().insert(child, mutation_context());
        txn->commit();
      }
      // Same key "k" on ancestor (not required) and child (required): child wins.
      const auto ancestor_def = make_cfd(10, lab_id, root.id, "k", /*required=*/false);
      const auto child_def = make_cfd(11, lab_id, child.id, "k", /*required=*/true);
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::CustomFieldDefinition>().insert(ancestor_def, mutation_context());
        txn->repo<core::CustomFieldDefinition>().insert(child_def, mutation_context());
        txn->commit();
      }

      std::vector<core::CustomFieldDefinition> resolved;
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        resolved = resolve_custom_field_defs(*txn, lab_id, child.id);
        txn->commit();
      }
      ASSERT_EQ(resolved.size(), 1U);
      EXPECT_EQ(resolved.front().key, "k");
      EXPECT_TRUE(resolved.front().required); // the child's tightened definition
    }

    TEST_P(SampleRepositoryTest, ResolveThenValidateRejectsMissingRequiredField) {
      const auto lab_id = seed_lab(1);
      const auto it_id = seed_item_type(3, lab_id);
      const auto required_def = make_cfd(10, lab_id, it_id, "patient_ref", /*required=*/true);
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        txn->repo<core::CustomFieldDefinition>().insert(required_def, mutation_context());
        txn->commit();
      }
      std::vector<core::CustomFieldDefinition> resolved;
      {
        auto txn = backend().begin(IsolationLevel::Serializable);
        resolved = resolve_custom_field_defs(*txn, lab_id, it_id);
        txn->commit();
      }
      const auto errors =
          core::validate_custom_fields(resolved, nlohmann::json::object()); // missing patient_ref
      EXPECT_FALSE(errors.empty());
    }

    INSTANTIATE_TEST_SUITE_P(Backends, SampleRepositoryTest,
                             ::testing::Values(BackendKind::Sqlite, BackendKind::Postgres),
                             [](const ::testing::TestParamInfo<BackendKind>& info) {
                               return backend_kind_name(info.param);
                             });

  } // namespace
} // namespace fmgr::storage
