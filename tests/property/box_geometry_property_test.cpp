// SPDX-License-Identifier: AGPL-3.0-or-later

// Property tests for box-geometry invariants (PRD §15): under an arbitrary
// sequence of sample moves, no two active samples ever occupy the same
// (box_id, position_label), and a move that fails leaves occupancy unchanged
// (atomic rollback). Driven against an in-memory SQLite backend.

#include "core/box.h"
#include "core/freezer.h"
#include "core/identity.h"
#include "core/item_type.h"
#include "core/sample.h"
#include "storage/BoxGeometryTraits.h"
#include "storage/FreezerTraits.h"
#include "storage/IdentityTraits.h"
#include "storage/ItemTypeTraits.h"
#include "storage/SampleOps.h"
#include "storage/SampleTraits.h"
#include "storage/sqlite/BoxGeometryRepositories.h"
#include "storage/sqlite/IdentityRepositories.h"
#include "storage/sqlite/ItemTypeRepositories.h"
#include "storage/sqlite/LayoutRepositories.h"
#include "storage/sqlite/SampleRepositories.h"
#include "storage/sqlite/SqliteBackend.h"

#include "test_helpers.h"
#include <gtest/gtest.h>
#include <rapidcheck.h>

#include <array>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace fmgr::storage {
  namespace {
    using namespace fmgr::test;

    constexpr std::size_t kSampleCount = 5;
    const std::array<std::string, 4> kPositions = {"A1", "A2", "A3", "A4"};
    const std::string kSizeClass = "cryovial_2ml";

    [[nodiscard]] MutationContext ctx() {
      return MutationContext{
          .actor_user_id = id_from_low<core::UserId>(999),
          .actor_session_id = "prop-session",
          .request_id = "prop-request",
          .reason = "box geometry property test",
      };
    }

    // An in-memory backend seeded with one box (positions A1..A4, all accepting
    // kSizeClass) and kSampleCount unplaced samples whose container type matches.
    class Fixture {
    public:
      Fixture() : backend_(storage::SqliteBackendOptions{.database_path = ":memory:"}) {
        register_identity_repositories(backend_);
        register_item_type_repositories(backend_);
        register_layout_repositories(backend_);
        register_box_geometry_repositories(backend_);
        register_box_repositories(backend_);
        register_sample_repositories(backend_);
        backend_.migrate_to_latest();
        seed();
      }

      storage::IStorageBackend& backend() {
        return backend_;
      }
      [[nodiscard]] core::BoxId box_id() const {
        return box_id_;
      }
      [[nodiscard]] core::SampleId sample(std::size_t index) const {
        return sample_ids_.at(index);
      }

    private:
      void seed() {
        const auto lab = id_from_low<core::LabId>(1);
        const auto user = id_from_low<core::UserId>(2);
        const auto item_type = id_from_low<core::ItemTypeId>(3);
        const auto container_type = id_from_low<core::ContainerTypeId>(4);
        const auto box_type = id_from_low<core::BoxTypeId>(5);
        const auto container = id_from_low<core::StorageContainerId>(6);
        box_id_ = id_from_low<core::BoxId>(7);

        // Cross-referencing entities are committed in dependency order because
        // validation reads committed rows.
        {
          auto txn = backend_.begin(IsolationLevel::Serializable);
          txn->repo<core::Lab>().insert(core::Lab{.id = lab,
                                                  .name = "Lab",
                                                  .contact = "lab@example.org",
                                                  .created_at = ts(100),
                                                  .settings_json = nlohmann::json::object()},
                                        ctx());
          txn->repo<core::User>().insert(
              core::User{.id = user,
                         .primary_email = "u@example.org",
                         .display_name = "U",
                         .status = core::UserStatus::Active,
                         .created_at = ts(200),
                         .auth_bindings = nlohmann::json::array({{{"provider", "local"}}}),
                         .default_lab_id = lab},
              ctx());
          txn->repo<core::ItemType>().insert(core::ItemType{.id = item_type,
                                                            .lab_id = lab,
                                                            .parent_id = std::nullopt,
                                                            .name = "liquid",
                                                            .created_at = ts(300)},
                                             ctx());
          txn->commit();
        }
        {
          auto txn = backend_.begin(IsolationLevel::Serializable);
          txn->repo<core::ContainerType>().insert(core::ContainerType{.id = container_type,
                                                                      .lab_id = lab,
                                                                      .name = "Cryovial",
                                                                      .size_class = kSizeClass,
                                                                      .material = "pp",
                                                                      .supplier_sku = "SKU",
                                                                      .created_at = ts(400)},
                                                  ctx());
          txn->commit();
        }
        {
          std::vector<core::Position> positions;
          for (std::size_t i = 0; i < kPositions.size(); ++i) {
            core::Position position{.label = kPositions.at(i),
                                    .row = 0,
                                    .col = static_cast<int>(i),
                                    .accepts = {kSizeClass}};
            positions.push_back(std::move(position));
          }
          auto txn = backend_.begin(IsolationLevel::Serializable);
          txn->repo<core::BoxType>().insert(core::BoxType{.id = box_type,
                                                          .lab_id = lab,
                                                          .name = "BoxType",
                                                          .manufacturer = "Generic",
                                                          .sku = "BT",
                                                          .positions = positions,
                                                          .created_at = ts(500)},
                                            ctx());
          txn->commit();
        }
        {
          auto txn = backend_.begin(IsolationLevel::Serializable);
          txn->repo<core::StorageContainer>().insert(
              core::StorageContainer{.id = container,
                                     .lab_id = lab,
                                     .parent_id = std::nullopt,
                                     .kind = core::ContainerKind::Shelf,
                                     .name = "Shelf",
                                     .ordering_index = 0,
                                     .created_at = ts(600)},
              ctx());
          txn->commit();
        }
        {
          auto txn = backend_.begin(IsolationLevel::Serializable);
          txn->repo<core::Box>().insert(core::Box{.id = box_id_,
                                                  .lab_id = lab,
                                                  .box_type_id = box_type,
                                                  .storage_container_id = container,
                                                  .label = "Box",
                                                  .created_at = ts(700)},
                                        ctx());
          txn->commit();
        }
        {
          auto txn = backend_.begin(IsolationLevel::Serializable);
          for (std::size_t i = 0; i < kSampleCount; ++i) {
            const auto sid = id_from_low<core::SampleId>(1000 + i);
            sample_ids_.push_back(sid);
            txn->repo<core::Sample>().insert(
                core::Sample{.id = sid,
                             .lab_id = lab,
                             .item_type_id = item_type,
                             .name = "S" + std::to_string(i),
                             .container_type_id = container_type,
                             .status = core::SampleStatus::Active,
                             .created_by = user,
                             .created_at = ts(800 + static_cast<std::int64_t>(i)),
                             .last_modified_by = user,
                             .last_modified_at = ts(800 + static_cast<std::int64_t>(i))},
                ctx());
          }
          txn->commit();
        }
      }

      storage::SqliteBackend backend_;
      core::BoxId box_id_;
      std::vector<core::SampleId> sample_ids_;
    };

    // Active samples currently in the box, by position. Asserts no double-booking.
    [[nodiscard]] std::set<std::string> occupied_positions(storage::IStorageBackend& backend,
                                                           core::BoxId box) {
      auto txn = backend.begin(IsolationLevel::Serializable);
      const auto samples = txn->repo<core::Sample>().query(storage::Query<core::Sample>::where(
          storage::field<core::Sample, std::string>(core::Sample::Field::BoxId) ==
          box.to_string()));
      txn->commit();
      std::set<std::string> seen;
      for (const auto& sample : samples) {
        RC_ASSERT(sample.position_label.has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access): guarded by RC_ASSERT above
        RC_ASSERT(seen.insert(sample.position_label.value()).second); // no duplicate position
      }
      return seen;
    }

    TEST(BoxGeometryProperty, RandomMovesNeverDoubleBook) {
      const bool passed = rc::check("random move sequence preserves position uniqueness", [] {
        Fixture fixture;
        const auto op_count = *rc::gen::inRange<int>(1, 20);
        for (int op = 0; op < op_count; ++op) {
          const auto sample_index = *rc::gen::inRange<std::size_t>(0, kSampleCount);
          // [0, kPositions): a position; == kPositions.size(): unplace.
          const auto target = *rc::gen::inRange<std::size_t>(0, kPositions.size() + 1);
          std::optional<core::BoxId> dest_box;
          std::optional<std::string> dest_position;
          if (target < kPositions.size()) {
            dest_box = fixture.box_id();
            dest_position = kPositions.at(target);
          }

          const auto before = occupied_positions(fixture.backend(), fixture.box_id());
          try {
            auto txn = fixture.backend().begin(IsolationLevel::Serializable);
            move_sample(*txn, fixture.sample(sample_index), dest_box, dest_position, ctx());
            txn->commit();
          } catch (const UniqueViolation&) {
            // Destination occupied: the move must have rolled back unchanged.
            RC_ASSERT(occupied_positions(fixture.backend(), fixture.box_id()) == before);
          }
          // Invariant holds after every op (occupied_positions asserts uniqueness).
          (void)occupied_positions(fixture.backend(), fixture.box_id());
        }
      });
      EXPECT_TRUE(passed);
    }

  } // namespace
} // namespace fmgr::storage
